#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "report_artifacts.h"

namespace aircannect {

struct ReportArtifactIndexRecord {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint64_t result_size = 0;
    uint64_t overview_size = 0;
    uint32_t result_crc32 = 0;
    uint32_t overview_crc32 = 0;
    uint32_t tile_offset = 0;
    uint16_t tile_count = 0;
};

struct ReportArtifactIndexInput {
    ReportArtifactKey key;
    uint64_t result_size = 0;
    uint64_t overview_size = 0;
    uint32_t result_crc32 = 0;
    uint32_t overview_crc32 = 0;
    const ReportRangeTileArtifact *tiles = nullptr;
    size_t tile_count = 0;
};

class ReportArtifactIndexBuilder;

class ReportArtifactIndex {
public:
    ~ReportArtifactIndex();

    ReportArtifactIndex(const ReportArtifactIndex &) = delete;
    ReportArtifactIndex &operator=(const ReportArtifactIndex &) = delete;

    size_t size() const { return record_count_; }
    size_t storage_bytes() const { return storage_bytes_; }

    const ReportArtifactIndexRecord *find(SleepDayId sleep_day) const;
    const ReportRangeTileArtifact *tiles(
        const ReportArtifactIndexRecord &record,
        size_t &count) const;

    bool availability(const ReportArtifactKey &request,
                      ReportArtifactAvailability &out) const;

private:
    ReportArtifactIndex() = default;

    bool allocate(size_t record_count, size_t tile_count);

    uint8_t *storage_ = nullptr;
    size_t storage_bytes_ = 0;
    ReportArtifactIndexRecord *records_ = nullptr;
    ReportRangeTileArtifact *tiles_ = nullptr;
    size_t record_count_ = 0;
    size_t tile_count_ = 0;

    friend class ReportArtifactIndexBuilder;
};

class ReportArtifactIndexBuilder {
public:
    static std::shared_ptr<const ReportArtifactIndex> build(
        const ReportArtifactIndexInput *inputs,
        size_t input_count);
    static std::shared_ptr<const ReportArtifactIndex> merge_availability(
        const ReportArtifactIndex &source,
        const ReportArtifactAvailability &availability);
    static std::shared_ptr<const ReportArtifactIndex> reconcile(
        const ReportArtifactIndex &source,
        const NightCatalog &catalog);

private:
    static std::shared_ptr<const ReportArtifactIndex> replace_input(
        const ReportArtifactIndex &source,
        const ReportArtifactIndexInput &input);
};

}  // namespace aircannect
