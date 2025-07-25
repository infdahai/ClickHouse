#include <Storages/MergeTree/MergeTreeDataPartCompact.h>
#include <DataTypes/NestedUtils.h>
#include <Storages/MergeTree/MergeTreeReaderCompactSingleBuffer.h>
#include <Storages/MergeTree/MergeTreeDataPartWriterCompact.h>
#include <Storages/MergeTree/LoadedMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Interpreters/Context.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int NO_FILE_IN_DATA_PART;
    extern const int BAD_SIZE_OF_FILE_IN_DATA_PART;
}

namespace MergeTreeSetting
{
    extern MergeTreeSettingsBool enable_index_granularity_compression;
}

MergeTreeDataPartCompact::MergeTreeDataPartCompact(
        const MergeTreeData & storage_,
        const String & name_,
        const MergeTreePartInfo & info_,
        const MutableDataPartStoragePtr & data_part_storage_,
        const IMergeTreeDataPart * parent_part_)
    : IMergeTreeDataPart(storage_, name_, info_, data_part_storage_, Type::Compact, parent_part_)
{
}

MergeTreeReaderPtr createMergeTreeReaderCompact(
    const MergeTreeDataPartInfoForReaderPtr & read_info,
    const NamesAndTypesList & columns_to_read,
    const StorageSnapshotPtr & storage_snapshot,
    const MarkRanges & mark_ranges,
    const VirtualFields & virtual_fields,
    UncompressedCache * uncompressed_cache,
    MarkCache * mark_cache,
    DeserializationPrefixesCache * deserialization_prefixes_cache,
    const MergeTreeReaderSettings & reader_settings,
    const ValueSizeMap & avg_value_size_hints,
    const ReadBufferFromFileBase::ProfileCallback & profile_callback)
{
    return std::make_unique<MergeTreeReaderCompactSingleBuffer>(
        read_info, columns_to_read, virtual_fields,
        storage_snapshot, uncompressed_cache,
        mark_cache, deserialization_prefixes_cache, mark_ranges, reader_settings,
        avg_value_size_hints, profile_callback, CLOCK_MONOTONIC_COARSE);
}

MergeTreeDataPartWriterPtr createMergeTreeDataPartCompactWriter(
    const String & data_part_name_,
    const String & logger_name_,
    const SerializationByName & serializations_,
    MutableDataPartStoragePtr data_part_storage_,
    const MergeTreeIndexGranularityInfo & index_granularity_info_,
    const MergeTreeSettingsPtr & storage_settings_,
    const NamesAndTypesList & columns_list,
    const ColumnPositions & column_positions,
    const StorageMetadataPtr & metadata_snapshot,
    const VirtualsDescriptionPtr & virtual_columns,
    const std::vector<MergeTreeIndexPtr> & indices_to_recalc,
    const ColumnsStatistics & stats_to_recalc_,
    const String & marks_file_extension_,
    const CompressionCodecPtr & default_codec_,
    const MergeTreeWriterSettings & writer_settings,
    MergeTreeIndexGranularityPtr computed_index_granularity)
{
    NamesAndTypesList ordered_columns_list;
    std::copy_if(columns_list.begin(), columns_list.end(), std::back_inserter(ordered_columns_list),
        [&column_positions](const auto & column) { return column_positions.contains(column.name); });

    /// Order of writing is important in compact format
    ordered_columns_list.sort([&column_positions](const auto & lhs, const auto & rhs)
        { return column_positions.at(lhs.name) < column_positions.at(rhs.name); });

    return std::make_unique<MergeTreeDataPartWriterCompact>(
        data_part_name_, logger_name_, serializations_, data_part_storage_,
        index_granularity_info_, storage_settings_, ordered_columns_list, metadata_snapshot, virtual_columns,
        indices_to_recalc, stats_to_recalc_, marks_file_extension_,
        default_codec_, writer_settings, std::move(computed_index_granularity));
}


void MergeTreeDataPartCompact::calculateEachColumnSizes(ColumnSizeByName & /*each_columns_size*/, ColumnSize & total_size) const
{
    auto bin_checksum = checksums.files.find(DATA_FILE_NAME_WITH_EXTENSION);
    if (bin_checksum != checksums.files.end())
    {
        total_size.data_compressed += bin_checksum->second.file_size;
        total_size.data_uncompressed += bin_checksum->second.uncompressed_size;
    }

    auto mrk_checksum = checksums.files.find(DATA_FILE_NAME + getMarksFileExtension());
    if (mrk_checksum != checksums.files.end())
        total_size.marks += mrk_checksum->second.file_size;
}

void MergeTreeDataPartCompact::loadIndexGranularityImpl(
    MergeTreeIndexGranularityPtr & index_granularity_ptr,
    const MergeTreeIndexGranularityInfo & index_granularity_info_,
    size_t marks_per_granule,
    const IDataPartStorage & data_part_storage_,
    const MergeTreeSettings & storage_settings)
{
    if (!index_granularity_info_.mark_type.adaptive)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "MergeTreeDataPartCompact cannot be created with non-adaptive granularity.");

    auto marks_file_path = index_granularity_info_.getMarksFilePath("data");

    std::unique_ptr<ReadBufferFromFileBase> buffer = data_part_storage_.readFileIfExists(marks_file_path, {}, {}, {});
    if (!buffer)
        throw Exception(
            ErrorCodes::NO_FILE_IN_DATA_PART,
            "Marks file '{}' doesn't exist",
            std::string(fs::path(data_part_storage_.getFullPath()) / marks_file_path));

    std::unique_ptr<ReadBuffer> marks_reader;
    bool marks_compressed = index_granularity_info_.mark_type.compressed;
    if (marks_compressed)
        marks_reader = std::make_unique<CompressedReadBufferFromFile>(std::move(buffer));
    else
        marks_reader = std::move(buffer);

    while (!marks_reader->eof())
    {
        marks_reader->ignore(marks_per_granule * sizeof(MarkInCompressedFile));
        size_t granularity;
        readBinaryLittleEndian(granularity, *marks_reader);
        index_granularity_ptr->appendMark(granularity);
    }

    if (storage_settings[MergeTreeSetting::enable_index_granularity_compression])
    {
        if (auto new_granularity_ptr = index_granularity_ptr->optimize())
            index_granularity_ptr = std::move(new_granularity_ptr);
    }
}

void MergeTreeDataPartCompact::loadIndexGranularity()
{
    if (columns.empty())
        throw Exception(ErrorCodes::NO_FILE_IN_DATA_PART, "No columns in part {}", name);

    loadIndexGranularityImpl(
        index_granularity,
        index_granularity_info,
        index_granularity_info.mark_type.with_substreams ? columns_substreams.getTotalSubstreams() : columns.size(),
        getDataPartStorage(),
        *storage.getSettings());
}

void MergeTreeDataPartCompact::loadMarksToCache(const Names & column_names, MarkCache * mark_cache) const
{
    if (column_names.empty() || !mark_cache)
        return;

    auto context = storage.getContext();
    auto read_settings = context->getReadSettings();
    auto * load_marks_threadpool = read_settings.load_marks_asynchronously ? &context->getLoadMarksThreadpool() : nullptr;
    auto info_for_read = std::make_shared<LoadedMergeTreeDataPartInfoForReader>(shared_from_this(), std::make_shared<AlterConversions>());

    LOG_TEST(getLogger("MergeTreeDataPartCompact"), "Loading marks into mark cache for columns {} of part {}", toString(column_names), name);

    MergeTreeMarksLoader loader(
        info_for_read,
        mark_cache,
        index_granularity_info.getMarksFilePath(DATA_FILE_NAME),
        index_granularity->getMarksCount(),
        index_granularity_info,
        /*save_marks_in_cache=*/ true,
        read_settings,
        load_marks_threadpool,
        index_granularity_info.mark_type.with_substreams ? columns_substreams.getTotalSubstreams() : columns.size());

    loader.loadMarks();
}

void MergeTreeDataPartCompact::removeMarksFromCache(MarkCache * mark_cache) const
{
    if (!mark_cache)
        return;

    auto mark_path = index_granularity_info.getMarksFilePath(DATA_FILE_NAME);
    auto key = MarkCache::hash(fs::path(getRelativePathOfActivePart()) / mark_path);
    mark_cache->remove(key);
}

bool MergeTreeDataPartCompact::hasColumnFiles(const NameAndTypePair & column) const
{
    if (!getColumnPosition(column.getNameInStorage()))
        return false;

    auto bin_checksum = checksums.files.find(DATA_FILE_NAME_WITH_EXTENSION);
    auto mrk_checksum = checksums.files.find(DATA_FILE_NAME + getMarksFileExtension());

    return (bin_checksum != checksums.files.end() && mrk_checksum != checksums.files.end());
}

std::optional<time_t> MergeTreeDataPartCompact::getColumnModificationTime(const String & /* column_name */) const
{
    return getDataPartStorage().getFileLastModified(DATA_FILE_NAME_WITH_EXTENSION).epochTime();
}

void MergeTreeDataPartCompact::doCheckConsistency(bool require_part_metadata) const
{
    String mrk_file_name = DATA_FILE_NAME + getMarksFileExtension();

    if (!checksums.empty())
    {
        /// count.txt should be present even in non custom-partitioned parts
        if (!checksums.files.contains("count.txt"))
            throw Exception(ErrorCodes::NO_FILE_IN_DATA_PART, "No checksum for count.txt");

        if (require_part_metadata)
        {
            if (!checksums.files.contains(mrk_file_name))
                throw Exception(
                    ErrorCodes::NO_FILE_IN_DATA_PART,
                    "No marks file checksum for column in part {}",
                    getDataPartStorage().getFullPath());
            if (!checksums.files.contains(DATA_FILE_NAME_WITH_EXTENSION))
                throw Exception(
                    ErrorCodes::NO_FILE_IN_DATA_PART,
                    "No data file checksum for in part {}",
                    getDataPartStorage().getFullPath());
        }
    }
    else
    {
        {
            /// count.txt should be present even in non custom-partitioned parts
            std::string file_path = "count.txt";
            if (!getDataPartStorage().existsFile(file_path) || getDataPartStorage().getFileSize(file_path) == 0)
                throw Exception(
                    ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART,
                    "Part {} is broken: {} is empty",
                    getDataPartStorage().getRelativePath(),
                    std::string(fs::path(getDataPartStorage().getFullPath()) / file_path));
        }

        /// Check that marks are nonempty and have the consistent size with columns number.

        if (getDataPartStorage().existsFile(mrk_file_name))
        {
            UInt64 file_size = getDataPartStorage().getFileSize(mrk_file_name);
             if (!file_size)
                throw Exception(
                    ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART,
                    "Part {} is broken: {} is empty.",
                    getDataPartStorage().getRelativePath(),
                    std::string(fs::path(getDataPartStorage().getFullPath()) / mrk_file_name));

            UInt64 expected_file_size = index_granularity_info.getMarkSizeInBytes(columns.size()) * index_granularity->getMarksCount();
            if (expected_file_size != file_size)
                throw Exception(
                    ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART,
                    "Part {} is broken: bad size of marks file '{}': {}, must be: {}",
                    getDataPartStorage().getRelativePath(),
                    std::string(fs::path(getDataPartStorage().getFullPath()) / mrk_file_name),
                    file_size, expected_file_size);
        }
    }
}

bool MergeTreeDataPartCompact::isStoredOnRemoteDisk() const
{
    return getDataPartStorage().isStoredOnRemoteDisk();
}

bool MergeTreeDataPartCompact::isStoredOnReadonlyDisk() const
{
    return getDataPartStorage().isReadonly();
}

bool MergeTreeDataPartCompact::isStoredOnRemoteDiskWithZeroCopySupport() const
{
    return getDataPartStorage().supportZeroCopyReplication();
}

MergeTreeDataPartCompact::~MergeTreeDataPartCompact()
{
    try
    {
        removeIfNeeded();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

}
