#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "large_allocator.h"
#include "night_catalog_builder.h"
#include "report_proto.h"
#include "report_spool_types.h"

namespace aircannect {

// Compact immutable Summary input retained by the report task. The backing
// arrays live in one PSRAM-capable allocation on esp32-s3 targets.
class NightCatalogSummarySnapshot {
public:
    ~NightCatalogSummarySnapshot();

    NightCatalogSummarySnapshot(const NightCatalogSummarySnapshot &) = delete;
    NightCatalogSummarySnapshot &operator=(
        const NightCatalogSummarySnapshot &) = delete;

    size_t size() const { return record_count_; }
    const NightCatalogSummaryInput *records() const { return records_; }

    static std::shared_ptr<const NightCatalogSummarySnapshot> build(
        const ReportSummaryRecord *records,
        size_t record_count,
        char *error = nullptr,
        size_t error_size = 0);
    static std::shared_ptr<const NightCatalogSummarySnapshot> copy(
        const NightCatalogSummaryInput *records,
        size_t record_count);
    static std::shared_ptr<const NightCatalogSummarySnapshot> parse(
        const ReportSpoolResult &result,
        char *error = nullptr,
        size_t error_size = 0);
    static std::shared_ptr<const NightCatalogSummarySnapshot> from_catalog(
        const NightCatalog &catalog);

    // Repair one recovered day without replacing newer retained Summary data.
    static std::shared_ptr<const NightCatalogSummarySnapshot> replace_night(
        const NightCatalogSummarySnapshot &current,
        const NightCatalog &single_night,
        uint64_t expected_summary_identity);

    static std::shared_ptr<const NightCatalogSummarySnapshot>
    preserve_expired_history(
        const NightCatalogSummarySnapshot &current,
        const NightCatalog &previous_catalog);

private:
    struct MaterializedRecord {
        SleepDayId sleep_day;
        int64_t day_start_ms = 0;
        int64_t day_end_ms = 0;
        ReportDailyMetrics metrics;
        uint64_t identity = 0;
        int32_t timezone_offset_minutes = 0;
        size_t session_count = 0;
        NightCatalogTimeRange sessions[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    };

    using MaterializedRecords =
        std::vector<MaterializedRecord, LargeAllocator<MaterializedRecord>>;

    struct ParseContext {
        MaterializedRecords *records = nullptr;
        bool allocation_failed = false;
    };

    NightCatalogSummarySnapshot() = default;

    static bool materialize_record(const ReportSummaryRecord &source,
                                   MaterializedRecord &target);
    static bool append_parsed_record(void *context,
                                     const ReportSummaryRecord &source);
    bool allocate(size_t record_count, size_t session_count);
    bool initialize_from_materialized(const MaterializedRecord *records,
                                      size_t record_count);

    uint8_t *storage_ = nullptr;
    size_t storage_bytes_ = 0;
    NightCatalogSummaryInput *records_ = nullptr;
    NightCatalogTimeRange *sessions_ = nullptr;
    size_t record_count_ = 0;
    size_t session_count_ = 0;
};

}  // namespace aircannect
