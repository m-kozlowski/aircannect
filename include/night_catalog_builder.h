#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "night_catalog.h"
#include "night_str_record.h"
#include "report_daily_metrics.h"

namespace aircannect {

class LargeByteBuffer;

struct NightCatalogSourceFileInput {
    NightCatalogFileKind kind = NightCatalogFileKind::Brp;
    const char *path = nullptr;
    NightCatalogSourceCoverage coverage;

    uint64_t file_size = 0;
    int64_t last_write_ms = 0;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    uint64_t identity = 0;
    int64_t record_start_ms = 0;

    uint32_t header_size = 0;
    uint32_t record_size = 0;
    uint32_t record_duration_ms = 0;
    uint32_t complete_records = 0;

    const EdfReportSignalLayout *signal_layouts = nullptr;
    size_t signal_layout_count = 0;
    uint64_t provenance_identity = 0;
};

struct NightCatalogEdfSessionInput {
    NightCatalogEdfSessionInput() = default;
    NightCatalogEdfSessionInput(
        SleepDayId sleep_day_value,
        int64_t day_start_value,
        int64_t day_end_value,
        NightCatalogTimeRange display_window_value,
        const NightCatalogSourceFileInput *files_value,
        size_t file_count_value)
        : sleep_day(sleep_day_value),
          day_start_ms(day_start_value),
          day_end_ms(day_end_value),
          display_window(display_window_value),
          files(files_value),
          file_count(file_count_value) {}

    SleepDayId sleep_day;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    NightCatalogTimeRange display_window;
    const NightCatalogSourceFileInput *files = nullptr;
    size_t file_count = 0;

    SleepDayId raw_sleep_day;
    NightCatalogTimeRange raw_segment_window;
    NightCatalogTimeRange raw_therapy_window;
    bool has_clock_provenance = false;
};

struct NightCatalogStrInput {
    NightStrRecord record;
    const char *path = nullptr;
    uint64_t file_size = 0;
    int64_t last_write_ms = 0;
    uint64_t record_offset = 0;
    uint32_t record_size = 0;
};

struct NightCatalogSummaryInput {
    SleepDayId sleep_day;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    const NightCatalogTimeRange *sessions = nullptr;
    size_t session_count = 0;
    ReportDailyMetrics metrics;
    uint64_t identity = 0;
};

struct NightCatalogFallbackSectionInput {
    ReportFallbackSectionKind kind = ReportFallbackSectionKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Count;
    uint8_t event_mask = 0;
    uint32_t payload_schema = 0;
    uint32_t record_count = 0;
    uint32_t sample_interval_ms = 0;
    NightCatalogTimeRange coverage;
    uint64_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t data_crc32 = 0;
};

struct NightCatalogFallbackInput {
    SleepDayId sleep_day;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    const NightCatalogTimeRange *sessions = nullptr;
    size_t session_count = 0;
    const char *path = nullptr;
    uint64_t file_size = 0;
    int64_t last_write_ms = 0;
    uint64_t identity = 0;
    uint32_t metadata_bytes = 0;
    const NightCatalogFallbackSectionInput *sections = nullptr;
    size_t section_count = 0;
};

struct NightCatalogBuildInput {
    const NightCatalogEdfSessionInput *edf_sessions = nullptr;
    size_t edf_session_count = 0;
    const NightCatalogStrInput *str_records = nullptr;
    size_t str_record_count = 0;
    const NightCatalogSummaryInput *summary_records = nullptr;
    size_t summary_record_count = 0;
    const NightCatalogFallbackInput *fallback_records = nullptr;
    size_t fallback_record_count = 0;
};

enum class NightCatalogBuildFailure : uint8_t {
    None,
    InvalidInput,
    AllocationFailed,
    InvariantViolation,
};

struct NightCatalogBuildStatus {
    NightCatalogBuildFailure failure = NightCatalogBuildFailure::None;
    const char *detail = "";

    bool retryable() const {
        return failure == NightCatalogBuildFailure::AllocationFailed;
    }
};

class NightCatalogBuilder {
public:
    static std::shared_ptr<const NightCatalog> build(
        const NightCatalogBuildInput &input,
        NightCatalogBuildStatus *status = nullptr);
    static std::shared_ptr<const NightCatalog> replace_fallback(
        const NightCatalog &catalog,
        const char *path,
        const std::shared_ptr<const LargeByteBuffer> &artifact,
        int64_t last_write_ms = 0);
};

}  // namespace aircannect
