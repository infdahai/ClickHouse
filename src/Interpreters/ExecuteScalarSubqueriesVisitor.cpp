#include <Interpreters/ExecuteScalarSubqueriesVisitor.h>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnTuple.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterSelectWithUnionQuery.h>
#include <Interpreters/ProcessorsProfileLog.h>
#include <Interpreters/addTypeConversionToAST.h>
#include <Interpreters/misc.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTWithElement.h>
#include <Processors/Executors/PullingAsyncPipelineExecutor.h>
#include <Common/FieldVisitorToString.h>
#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
extern const Event ScalarSubqueriesGlobalCacheHit;
extern const Event ScalarSubqueriesLocalCacheHit;
extern const Event ScalarSubqueriesCacheMiss;
}

namespace DB
{
namespace Setting
{
    extern const SettingsBool enable_scalar_subquery_optimization;
    extern const SettingsBool extremes;
    extern const SettingsUInt64 max_result_rows;
    extern const SettingsBool use_concurrency_control;
    extern const SettingsString implicit_table_at_top_level;
}

namespace ErrorCodes
{
    extern const int INCORRECT_RESULT_OF_SCALAR_SUBQUERY;
}


bool ExecuteScalarSubqueriesMatcher::needChildVisit(ASTPtr & node, const ASTPtr & child)
{
    /// Processed
    if (node->as<ASTSubquery>() || node->as<ASTFunction>())
        return false;

    /// Don't descend into subqueries in FROM section
    if (node->as<ASTTableExpression>())
        return false;

    /// Do not go to subqueries defined in with statement
    if (node->as<ASTWithElement>())
        return false;

    if (node->as<ASTSelectQuery>())
    {
        /// Do not go to FROM, JOIN, UNION.
        if (child->as<ASTTableExpression>() || child->as<ASTSelectQuery>())
            return false;
    }

    if (auto * tables = node->as<ASTTablesInSelectQueryElement>())
    {
        /// Contrary to what's said in the code block above, ARRAY JOIN needs to resolve the subquery if possible
        /// and assign an alias for 02367_optimize_trivial_count_with_array_join to pass. Otherwise it will fail in
        /// ArrayJoinedColumnsVisitor (`No alias for non-trivial value in ARRAY JOIN: _a`)
        /// This looks 100% as a incomplete code working on top of a bug, but this code has already been made obsolete
        /// by the new analyzer, so it's an inconvenience we can live with until we deprecate it.
        if (child == tables->array_join)
            return true;
        return false;
    }

    return true;
}

void ExecuteScalarSubqueriesMatcher::visit(ASTPtr & ast, Data & data)
{
    if (const auto * t = ast->as<ASTSubquery>())
        visit(*t, ast, data);
    if (const auto * t = ast->as<ASTFunction>())
        visit(*t, ast, data);
}

static auto getQueryInterpreter(const ASTSubquery & subquery, ExecuteScalarSubqueriesMatcher::Data & data)
{
    auto subquery_context = Context::createCopy(data.getContext());
    Settings subquery_settings = data.getContext()->getSettingsCopy();
    subquery_settings[Setting::max_result_rows] = 1;
    subquery_settings[Setting::extremes] = false;
    subquery_settings[Setting::implicit_table_at_top_level] = "";
    subquery_context->setSettings(subquery_settings);

    if (subquery_context->hasQueryContext())
    {
        /// When execute `INSERT INTO t WITH ... SELECT ...`, it may lead to `Unknown columns`
        /// exception with this settings enabled(https://github.com/ClickHouse/ClickHouse/issues/52494).
        subquery_context->getQueryContext()->setSetting("use_structure_from_insertion_table_in_table_functions", false);
        if (!data.only_analyze)
        {
            /// Save current cached scalars in the context before analyzing the query
            /// This is specially helpful when analyzing CTE scalars
            auto context = subquery_context->getQueryContext();
            for (const auto & it : data.scalars)
                context->addScalar(it.first, it.second);
        }
    }

    ASTPtr subquery_select = subquery.children.at(0);

    auto options = SelectQueryOptions(QueryProcessingStage::Complete, data.subquery_depth + 1, true);
    options.is_create_parameterized_view = data.is_create_parameterized_view;
    options.analyze(data.only_analyze);

    return std::make_unique<InterpreterSelectWithUnionQuery>(subquery_select, subquery_context, options);
}

void ExecuteScalarSubqueriesMatcher::visit(const ASTSubquery & subquery, ASTPtr & ast, Data & data)
{
    /// subquery and ast can be the same object and ast will be moved.
    /// Save these fields to avoid use after move.
    String subquery_alias = subquery.alias;
    bool prefer_alias_to_column_name = subquery.prefer_alias_to_column_name;

    auto hash = subquery.getTreeHash(/*ignore_aliases=*/ true);
    const auto scalar_query_hash_str = toString(hash);

    std::unique_ptr<InterpreterSelectWithUnionQuery> interpreter;
    bool hit = false;
    bool is_local = false;

    Block scalar;
    if (data.only_analyze)
    {
        /// Don't use scalar cache during query analysis
    }
    else if (data.local_scalars.contains(scalar_query_hash_str))
    {
        hit = true;
        scalar = data.local_scalars[scalar_query_hash_str];
        is_local = true;
        ProfileEvents::increment(ProfileEvents::ScalarSubqueriesLocalCacheHit);
    }
    else if (data.scalars.contains(scalar_query_hash_str))
    {
        hit = true;
        scalar = data.scalars[scalar_query_hash_str];
        ProfileEvents::increment(ProfileEvents::ScalarSubqueriesGlobalCacheHit);
    }
    else
    {
        if (data.getContext()->hasQueryContext() && data.getContext()->getQueryContext()->hasScalar(scalar_query_hash_str))
        {
            if (!data.getContext()->getViewSource())
            {
                /// We aren't using storage views so we can safely use the context cache
                scalar = data.getContext()->getQueryContext()->getScalar(scalar_query_hash_str);
                ProfileEvents::increment(ProfileEvents::ScalarSubqueriesGlobalCacheHit);
                hit = true;
            }
            else
            {
                /// If we are under a context that uses views that means that the cache might contain values that reference
                /// the original table and not the view, so in order to be able to check the global cache we need to first
                /// make sure that the query doesn't use the view
                /// Note in any case the scalar will end up cached in *data* so this won't be repeated inside this context
                interpreter = getQueryInterpreter(subquery, data);
                if (!interpreter->usesViewSource())
                {
                    scalar = data.getContext()->getQueryContext()->getScalar(scalar_query_hash_str);
                    ProfileEvents::increment(ProfileEvents::ScalarSubqueriesGlobalCacheHit);
                    hit = true;
                }
            }
        }
    }

    if (!hit)
    {
        if (!interpreter)
            interpreter = getQueryInterpreter(subquery, data);

        ProfileEvents::increment(ProfileEvents::ScalarSubqueriesCacheMiss);
        is_local = interpreter->usesViewSource();

        Block block;

        if (data.only_analyze)
        {
            /// If query is only analyzed, then constants are not correct.
            block = *interpreter->getSampleBlock();
            for (auto & column : block)
            {
                if (column.column->empty())
                {
                    auto mut_col = column.column->cloneEmpty();
                    mut_col->insertDefault();
                    column.column = std::move(mut_col);
                }
            }
        }
        else
        {
            auto io = interpreter->execute();

            PullingAsyncPipelineExecutor executor(io.pipeline);
            io.pipeline.setProgressCallback(data.getContext()->getProgressCallback());
            io.pipeline.setConcurrencyControl(data.getContext()->getSettingsRef()[Setting::use_concurrency_control]);
            while (block.rows() == 0 && executor.pull(block))
            {
            }

            if (block.rows() == 0)
            {
                auto types = interpreter->getSampleBlock()->getDataTypes();
                if (types.size() != 1)
                    types = {std::make_shared<DataTypeTuple>(types)};

                auto & type = types[0];
                if (!type->isNullable())
                {
                    if (!type->canBeInsideNullable())
                        throw Exception(ErrorCodes::INCORRECT_RESULT_OF_SCALAR_SUBQUERY,
                                        "Scalar subquery returned empty result of type {} which cannot be Nullable",
                                        type->getName());

                    type = makeNullable(type);
                }

                ASTPtr ast_new = std::make_shared<ASTLiteral>(Null());
                ast_new = addTypeConversionToAST(std::move(ast_new), type->getName());

                ast_new->setAlias(ast->tryGetAlias());
                ast = std::move(ast_new);

                /// Empty subquery result is equivalent to NULL
                block = interpreter->getSampleBlock()->cloneEmpty();
                String column_name = block.columns() > 0 ?  block.safeGetByPosition(0).name : "dummy";
                block = Block({
                    ColumnWithTypeAndName(type->createColumnConstWithDefaultValue(1)->convertToFullColumnIfConst(), type, column_name)
                });
            }

            if (block.rows() != 1)
                throw Exception(ErrorCodes::INCORRECT_RESULT_OF_SCALAR_SUBQUERY, "Scalar subquery returned more than one row");

            Block tmp_block;
            while (tmp_block.rows() == 0 && executor.pull(tmp_block))
            {
            }

            if (tmp_block.rows() != 0)
                throw Exception(ErrorCodes::INCORRECT_RESULT_OF_SCALAR_SUBQUERY, "Scalar subquery returned more than one row");

            logProcessorProfile(data.getContext(), io.pipeline.getProcessors());
        }

        block = materializeBlock(block);
        size_t columns = block.columns();

        if (columns == 1)
        {
            auto & column = block.getByPosition(0);
            /// Here we wrap type to nullable if we can.
            /// It is needed cause if subquery return no rows, it's result will be Null.
            /// In case of many columns, do not check it cause tuple can't be nullable.
            if (!column.type->isNullable() && column.type->canBeInsideNullable())
            {
                column.type = makeNullable(column.type);
                column.column = makeNullable(column.column);
            }
            scalar = block;
        }
        else
        {
            scalar.insert({
                ColumnTuple::create(block.getColumns()),
                std::make_shared<DataTypeTuple>(block.getDataTypes()),
                "tuple"});
        }
    }

    const Settings & settings = data.getContext()->getSettingsRef();

    // Always convert to literals when there is no query context.
    if (data.only_analyze || !settings[Setting::enable_scalar_subquery_optimization] || worthConvertingScalarToLiteral(scalar, data.max_literal_size)
        || !data.getContext()->hasQueryContext())
    {
        auto lit = std::make_unique<ASTLiteral>((*scalar.safeGetByPosition(0).column)[0]);
        lit->alias = subquery_alias;
        lit->prefer_alias_to_column_name = prefer_alias_to_column_name;
        ast = addTypeConversionToAST(std::move(lit), scalar.safeGetByPosition(0).type->getName());

        /// If only analyze was requested the expression is not suitable for constant folding, disable it.
        if (data.only_analyze)
        {
            ast->as<ASTFunction>()->alias.clear();
            auto func = makeASTFunction("__scalarSubqueryResult", std::move(ast));
            func->alias = subquery_alias;
            func->prefer_alias_to_column_name = prefer_alias_to_column_name;
            ast = std::move(func);
        }
    }
    else if (!data.replace_only_to_literals)
    {
        auto func = makeASTFunction("__getScalar", std::make_shared<ASTLiteral>(scalar_query_hash_str));
        func->alias = subquery_alias;
        func->prefer_alias_to_column_name = prefer_alias_to_column_name;
        ast = std::move(func);
    }

    if (is_local)
        data.local_scalars[scalar_query_hash_str] = std::move(scalar);
    else
        data.scalars[scalar_query_hash_str] = std::move(scalar);
}

void ExecuteScalarSubqueriesMatcher::visit(const ASTFunction & func, ASTPtr & ast, Data & data)
{
    /// Don't descend into subqueries in arguments of IN operator.
    /// But if an argument is not subquery, then deeper may be scalar subqueries and we need to descend in them.

    std::vector<ASTPtr *> out;
    if (checkFunctionIsInOrGlobalInOperator(func))
    {
        for (auto & child : ast->children)
        {
            if (child != func.arguments)
                out.push_back(&child);
            else
                for (size_t i = 0, size = func.arguments->children.size(); i < size; ++i)
                    if (i != 1 || !func.arguments->children[i]->as<ASTSubquery>())
                        out.push_back(&func.arguments->children[i]);
        }
    }
    else
        for (auto & child : ast->children)
            out.push_back(&child);

    for (ASTPtr * add_node : out)
        Visitor(data).visit(*add_node);
}

static size_t getSizeOfSerializedLiteral(const Field & field)
{
    auto field_str = applyVisitor(FieldVisitorToString(), field);
    return field_str.size();
}

bool worthConvertingScalarToLiteral(const Block & scalar, std::optional<size_t> max_literal_size)
{
    /// Converting to literal values might take a fair amount of overhead when the value is large, (e.g.
    /// Array, BitMap, etc.), This conversion is required for constant folding, index lookup, branch
    /// elimination. However, these optimizations should never be related to large values, thus we blacklist them here.
    const auto * scalar_type_name = scalar.safeGetByPosition(0).type->getFamilyName();
    static const std::set<std::string_view> maybe_large_literal_types = {"Array", "Tuple", "AggregateFunction", "Function", "Set", "LowCardinality"};

    if (!maybe_large_literal_types.contains(scalar_type_name))
        return true;

    if (!max_literal_size)
        return false;

    /// Size of serialized literal cannot be less than size in bytes.
    if (scalar.bytes() > *max_literal_size)
        return false;

    return getSizeOfSerializedLiteral((*scalar.safeGetByPosition(0).column)[0]) <= *max_literal_size;
}

}
