#pragma once

#include <Columns/IColumn.h>
#include <Columns/ColumnsNumber.h>
#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>

#include "config.h"


class Collator;

namespace DB
{

using NullMap = ColumnUInt8::Container;
using ConstNullMapPtr = const NullMap *;

/// Class that specifies nullable columns. A nullable column represents
/// a column, which may have any type, provided with the possibility of
/// storing NULL values. For this purpose, a ColumNullable object stores
/// an ordinary column along with a special column, namely a byte map,
/// whose type is ColumnUInt8. The latter column indicates whether the
/// value of a given row is a NULL or not. Such a design is preferred
/// over a bitmap because columns are usually stored on disk as compressed
/// files. In this regard, using a bitmap instead of a byte map would
/// greatly complicate the implementation with little to no benefits.
class ColumnNullable final : public COWHelper<IColumnHelper<ColumnNullable>, ColumnNullable>
{
private:
    friend class COWHelper<IColumnHelper<ColumnNullable>, ColumnNullable>;

    ColumnNullable(MutableColumnPtr && nested_column_, MutableColumnPtr && null_map_);
    ColumnNullable(const ColumnNullable &) = default;

public:
    /** Create immutable column using immutable arguments. This arguments may be shared with other columns.
      * Use IColumn::mutate in order to make mutable column and mutate shared nested columns.
      */
    using Base = COWHelper<IColumnHelper<ColumnNullable>, ColumnNullable>;
    static Ptr create(const ColumnPtr & nested_column_, const ColumnPtr & null_map_)
    {
        return ColumnNullable::create(nested_column_->assumeMutable(), null_map_->assumeMutable());
    }

    template <typename ... Args>
    requires (IsMutableColumns<Args ...>::value)
    static MutablePtr create(Args &&... args) { return Base::create(std::forward<Args>(args)...); }

    const char * getFamilyName() const override { return "Nullable"; }
    std::string getName() const override { return "Nullable(" + nested_column->getName() + ")"; }
    TypeIndex getDataType() const override { return TypeIndex::Nullable; }
    MutableColumnPtr cloneResized(size_t size) const override;
    size_t size() const override { return assert_cast<const ColumnUInt8 &>(*null_map).size(); }
    bool isNullAt(size_t n) const override { return assert_cast<const ColumnUInt8 &>(*null_map).getData()[n] != 0;}
    Field operator[](size_t n) const override;
    void get(size_t n, Field & res) const override;
    std::pair<String, DataTypePtr> getValueNameAndType(size_t n) const override;
    bool getBool(size_t n) const override { return isNullAt(n) ? false : nested_column->getBool(n); }
    UInt64 get64(size_t n) const override { return nested_column->get64(n); }
    Float64 getFloat64(size_t n) const override;
    Float32 getFloat32(size_t n) const override;
    UInt64 getUInt(size_t n) const override;
    Int64 getInt(size_t n) const override;
    bool isDefaultAt(size_t n) const override { return isNullAt(n); }
    StringRef getDataAt(size_t) const override;
    /// Will insert null value if pos=nullptr
    void insertData(const char * pos, size_t length) override;
    StringRef serializeValueIntoArena(size_t n, Arena & arena, char const *& begin) const override;
    char * serializeValueIntoMemory(size_t n, char * memory) const override;
    const char * deserializeAndInsertFromArena(const char * pos) override;
    const char * skipSerializedInArena(const char * pos) const override;
#if !defined(DEBUG_OR_SANITIZER_BUILD)
    void insertRangeFrom(const IColumn & src, size_t start, size_t length) override;
#else
    void doInsertRangeFrom(const IColumn & src, size_t start, size_t length) override;
#endif
    void insert(const Field & x) override;
    bool tryInsert(const Field & x) override;

#if !defined(DEBUG_OR_SANITIZER_BUILD)
    void insertFrom(const IColumn & src, size_t n) override;
    void insertManyFrom(const IColumn & src, size_t position, size_t length) override;
#else
    void doInsertFrom(const IColumn & src, size_t n) override;
    void doInsertManyFrom(const IColumn & src, size_t position, size_t length) override;
#endif

    void insertFromNotNullable(const IColumn & src, size_t n);
    void insertRangeFromNotNullable(const IColumn & src, size_t start, size_t length);
    void insertManyFromNotNullable(const IColumn & src, size_t position, size_t length);

    void insertDefault() override
    {
        getNestedColumn().insertDefault();
        getNullMapData().push_back(1);
    }

    void popBack(size_t n) override;
    ColumnPtr filter(const Filter & filt, ssize_t result_size_hint) const override;
    void expand(const Filter & mask, bool inverted) override;
    ColumnPtr permute(const Permutation & perm, size_t limit) const override;
    ColumnPtr index(const IColumn & indexes, size_t limit) const override;
#if !defined(DEBUG_OR_SANITIZER_BUILD)
    int compareAt(size_t n, size_t m, const IColumn & rhs_, int null_direction_hint) const override;
#else
    int doCompareAt(size_t n, size_t m, const IColumn & rhs_, int null_direction_hint) const override;
#endif

#if USE_EMBEDDED_COMPILER

    bool isComparatorCompilable() const override;

    llvm::Value * compileComparator(llvm::IRBuilderBase & /*builder*/, llvm::Value * /*lhs*/, llvm::Value * /*rhs*/, llvm::Value * /*nan_direction_hint*/) const override;

#endif

    int compareAtWithCollation(size_t n, size_t m, const IColumn & rhs, int null_direction_hint, const Collator &) const override;
    void getPermutation(IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                        size_t limit, int null_direction_hint, Permutation & res) const override;
    void updatePermutation(IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                        size_t limit, int null_direction_hint, Permutation & res, EqualRanges & equal_ranges) const override;
    void getPermutationWithCollation(const Collator & collator, IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                        size_t limit, int null_direction_hint, Permutation & res) const override;
    void updatePermutationWithCollation(const Collator & collator, IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                        size_t limit, int null_direction_hint, Permutation & res, EqualRanges& equal_ranges) const override;
    size_t estimateCardinalityInPermutedRange(const Permutation & permutation, const EqualRange & equal_range) const override;
    void reserve(size_t n) override;
    size_t capacity() const override;
    void prepareForSquashing(const Columns & source_columns, size_t factor) override;
    void shrinkToFit() override;
    void ensureOwnership() override;
    size_t byteSize() const override;
    size_t byteSizeAt(size_t n) const override;
    size_t allocatedBytes() const override;
    void protect() override;
    ColumnPtr replicate(const Offsets & replicate_offsets) const override;
    void updateHashWithValue(size_t n, SipHash & hash) const override;
    WeakHash32 getWeakHash32() const override;
    void updateHashFast(SipHash & hash) const override;
    void getExtremes(Field & min, Field & max) const override;
    // Special function for nullable minmax index
    void getExtremesNullLast(Field & min, Field & max) const;

    ColumnPtr compress(bool force_compression) const override;

    ColumnCheckpointPtr getCheckpoint() const override;
    void updateCheckpoint(ColumnCheckpoint & checkpoint) const override;
    void rollback(const ColumnCheckpoint & checkpoint) override;

    void forEachMutableSubcolumn(MutableColumnCallback callback) override
    {
        callback(nested_column);
        callback(null_map);
    }

    void forEachMutableSubcolumnRecursively(RecursiveMutableColumnCallback callback) override
    {
        callback(*nested_column);
        nested_column->forEachMutableSubcolumnRecursively(callback);
        callback(*null_map);
        null_map->forEachMutableSubcolumnRecursively(callback);
    }

    void forEachSubcolumn(ColumnCallback callback) const override
    {
        callback(nested_column);
        callback(null_map);
    }

    void forEachSubcolumnRecursively(RecursiveColumnCallback callback) const override
    {
        callback(*nested_column);
        nested_column->forEachSubcolumnRecursively(callback);
        callback(*null_map);
        null_map->forEachSubcolumnRecursively(callback);
    }

    bool structureEquals(const IColumn & rhs) const override
    {
        if (const auto * rhs_nullable = typeid_cast<const ColumnNullable *>(&rhs))
            return nested_column->structureEquals(*rhs_nullable->nested_column);
        return false;
    }

    ColumnPtr createWithOffsets(const Offsets & offsets, const ColumnConst & column_with_default_value, size_t total_rows, size_t shift) const override;
    void updateAt(const IColumn & src, size_t dst_pos, size_t src_pos) override;

    bool isNullable() const override { return true; }
    bool isFixedAndContiguous() const override { return false; }
    bool valuesHaveFixedSize() const override { return nested_column->valuesHaveFixedSize(); }
    size_t sizeOfValueIfFixed() const override { return null_map->sizeOfValueIfFixed() + nested_column->sizeOfValueIfFixed(); }
    bool onlyNull() const override { return nested_column->isDummy(); }
    bool isCollationSupported() const override { return nested_column->isCollationSupported(); }


    /// Return the column that represents values.
    IColumn & getNestedColumn() { return *nested_column; }
    const IColumn & getNestedColumn() const { return *nested_column; }

    const ColumnPtr & getNestedColumnPtr() const { return nested_column; }
    ColumnPtr & getNestedColumnPtr() { return nested_column; }

    /// Return the column that represents the byte map.
    const ColumnPtr & getNullMapColumnPtr() const { return null_map; }
    ColumnPtr & getNullMapColumnPtr() { return null_map; }

    ColumnUInt8 & getNullMapColumn() { return assert_cast<ColumnUInt8 &>(*null_map); }
    const ColumnUInt8 & getNullMapColumn() const { return assert_cast<const ColumnUInt8 &>(*null_map); }

    NullMap & getNullMapData() { return getNullMapColumn().getData(); }
    const NullMap & getNullMapData() const { return getNullMapColumn().getData(); }

    ColumnPtr getNestedColumnWithDefaultOnNull() const;

    /// Apply the null byte map of a specified nullable column onto the
    /// null byte map of the current column by performing an element-wise OR
    /// between both byte maps. This method is used to determine the null byte
    /// map of the result column of a function taking one or more nullable
    /// columns.
    void applyNullMap(const ColumnNullable & other);
    void applyNullMap(const ColumnUInt8 & map);
    void applyNullMap(const NullMap & map);
    void applyNegatedNullMap(const ColumnUInt8 & map);
    void applyNegatedNullMap(const NullMap & map);

    /// Check that size of null map equals to size of nested column.
    void checkConsistency() const;

    bool hasDynamicStructure() const override { return nested_column->hasDynamicStructure(); }
    void takeDynamicStructureFromSourceColumns(const Columns & source_columns) override;

private:
    WrappedPtr nested_column;
    WrappedPtr null_map;

    template <bool negative>
    void applyNullMapImpl(const NullMap & map);

    int compareAtImpl(size_t n, size_t m, const IColumn & rhs_, int null_direction_hint, const Collator * collator=nullptr) const;

    void getPermutationImpl(IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                        size_t limit, int null_direction_hint, Permutation & res, const Collator * collator = nullptr) const;

    void updatePermutationImpl(IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                            size_t limit, int null_direction_hint, Permutation & res, EqualRanges & equal_ranges, const Collator * collator = nullptr) const;
};

ColumnPtr makeNullable(const ColumnPtr & column);
ColumnPtr makeNullableSafe(const ColumnPtr & column);
ColumnPtr makeNullableOrLowCardinalityNullable(const ColumnPtr & column);
ColumnPtr makeNullableOrLowCardinalityNullableSafe(const ColumnPtr & column);

ColumnPtr removeNullable(const ColumnPtr & column);
ColumnPtr removeNullableOrLowCardinalityNullable(const ColumnPtr & column);

}
