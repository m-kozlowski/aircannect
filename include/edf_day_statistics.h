#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "edf_file_reader.h"
#include "edf_report_catalog.h"
#include "edf_session_metadata.h"
#include "operation_outcome.h"
#include "report_records.h"
#include "sleep_day_id.h"

namespace aircannect {

static constexpr size_t AC_EDF_DAY_STATISTICS_SIGNAL_MAX = 24;
static constexpr size_t AC_EDF_DAY_STATISTICS_COVERAGE_MAX = 64;
static constexpr size_t AC_EDF_DAY_STATISTICS_EVENT_MAX = 4096;
static constexpr size_t AC_EDF_DAY_STATISTICS_SESSION_MAX = 24;

enum class EdfDayStatisticsState : uint8_t {
    Idle,
    SubmitScan,
    WaitScan,
    SelectFile,
    SubmitHeader,
    WaitHeader,
    SubmitMetadata,
    WaitMetadata,
    SubmitRecord,
    WaitRecord,
    Finalize,
    Complete,
    Failed,
    Cancelled,
};

enum class EdfDayStatisticsError : uint8_t {
    None,
    InvalidArgument,
    AllocationFailed,
    ScanRejected,
    ScanFailed,
    ReadRejected,
    ReadFailed,
    InvalidHeader,
    RecordTooLarge,
    StatisticsUnavailable,
};

enum EdfDayStatisticsWarning : uint32_t {
    EDF_DAY_STATISTICS_WARNING_NONE = 0,
    EDF_DAY_STATISTICS_WARNING_METADATA_INVALID = 1u << 0,
    EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED = 1u << 1,
    EDF_DAY_STATISTICS_WARNING_COVERAGE_TRUNCATED = 1u << 2,
    EDF_DAY_STATISTICS_WARNING_EVENTS_TRUNCATED = 1u << 3,
};

struct EdfDayCoverageInterval {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

struct EdfDayCoverage {
    size_t count = 0;
    bool truncated = false;
    EdfDayCoverageInterval intervals[AC_EDF_DAY_STATISTICS_COVERAGE_MAX] = {};
};

struct EdfDaySignalStatistics {
    ReportSignalId signal = ReportSignalId::Invalid;
    ReportSourceId source = ReportSourceId::Summary;
    EdfSignalScale scale;
    bool primary = false;
    uint64_t sample_count = 0;
    uint64_t weighted_duration_ms = 0;
    bool valid = false;
    bool includes_fallback = false;
    uint16_t source_mask = 0;
    int32_t min_milli = 0;
    int32_t p5_milli = 0;
    int32_t p50_milli = 0;
    int32_t p70_milli = 0;
    int32_t p95_milli = 0;
    int32_t max_milli = 0;
    EdfDayCoverage coverage;
};

struct EdfDayEventCoverage {
    EdfInventoryFileKind kind = EdfInventoryFileKind::Unknown;
    EdfDayCoverage coverage;
};

struct EdfDayStatisticsResult {
    SleepDayId day;
    int64_t window_start_ms = 0;
    int64_t window_end_ms = 0;
    size_t signal_count = 0;
    EdfDaySignalStatistics signals[AC_EDF_DAY_STATISTICS_SIGNAL_MAX] = {};
    EdfDayEventCoverage event_coverage[2] = {};
    uint32_t event_counts[8] = {};
    size_t event_count = 0;
    bool events_truncated = false;
    ReportEventRecord events[AC_EDF_DAY_STATISTICS_EVENT_MAX] = {};
    bool provenance_used = false;
    size_t provenance_session_count = 0;
    bool provenance_sessions_truncated = false;
    EdfSessionMetadata provenance_sessions[
        AC_EDF_DAY_STATISTICS_SESSION_MAX] = {};
    uint32_t warnings = EDF_DAY_STATISTICS_WARNING_NONE;
    uint64_t records_processed = 0;
};

struct EdfDayStatisticsStatus {
    EdfDayStatisticsState state = EdfDayStatisticsState::Idle;
    EdfDayStatisticsError error = EdfDayStatisticsError::None;
    uint32_t generation = 0;
    size_t files_seen = 0;
    size_t files_processed = 0;
    uint64_t records_processed = 0;
    char error_text[48] = {};

    bool active() const {
        return state != EdfDayStatisticsState::Idle &&
               state != EdfDayStatisticsState::Complete &&
               state != EdfDayStatisticsState::Failed &&
               state != EdfDayStatisticsState::Cancelled;
    }

    bool terminal() const {
        return state == EdfDayStatisticsState::Complete ||
               state == EdfDayStatisticsState::Failed ||
               state == EdfDayStatisticsState::Cancelled;
    }
};

class StorageReadPort;
class StorageScanPort;
struct EdfReportSeriesSpan;

class EdfDayStatisticsReader final {
public:
    EdfDayStatisticsReader();
    ~EdfDayStatisticsReader();

    EdfDayStatisticsReader(const EdfDayStatisticsReader &) = delete;
    EdfDayStatisticsReader &operator=(const EdfDayStatisticsReader &) = delete;

    void begin(StorageReadPort &read_port, StorageScanPort &scan_port);
    OperationAdmission start(SleepDayId day, int32_t timezone_offset_minutes);

    // One poll performs at most one storage transition or one EDF record.
    bool poll();
    void cancel();
    void reset();

    // original_physical uses the source unit of the local EDF signal. For
    // Leak this is L/s; the resulting *_milli values use report units (L/min).
    // Only the part not covered by local EDF samples is added.
    bool add_fallback_sample(ReportSignalId signal,
                             float original_physical,
                             int64_t start_ms,
                             uint32_t duration_ms);
    bool finish_statistics();

    const EdfDayStatisticsStatus &status() const { return status_; }
    const EdfDayStatisticsResult &result() const;

private:
    struct Runtime;

    bool submit_scan();
    bool take_scan();
    bool select_file();
    bool submit_header();
    bool submit_metadata();
    int metadata_slot_for_current() const;
    bool submit_record();
    bool take_read();
    bool describe_current_file(const uint8_t *header, size_t length);
    bool register_current_numeric_session();
    bool resolve_current_annotation_session();
    bool apply_current_metadata();
    bool prepare_current_file();
    bool decode_current_record(const uint8_t *record, size_t length);
    bool finish_current_file();
    bool advance_after_file();
    bool finish_cancel();
    bool finish_statistics_step();
    bool add_fallback_segment(EdfDaySignalStatistics &statistics,
                              void *histogram,
                              int64_t start_ms,
                              int64_t end_ms,
                              float original_physical);
    void update_result_provenance_end();
    void fail(EdfDayStatisticsError error, const char *text);
    void complete();
    void append_provenance(const EdfSessionMetadata &metadata);
    void clear_read_ticket();
    void release_read_prepared();

    static bool accept_series_span(void *context,
                                   const EdfReportSeriesSpan &span);
    static bool accept_event(void *context,
                             const ReportEventRecord &event);

    Runtime *runtime_ = nullptr;
    StorageReadPort *read_port_ = nullptr;
    StorageScanPort *scan_port_ = nullptr;
    EdfDayStatisticsStatus status_;
};

// Maps a history timestamp in the recorder's raw clock into the canonical
// timeline used by EDF and STR, using the metadata records read for the day.
bool edf_day_statistics_map_raw_time(const EdfDayStatisticsResult &result,
                                     int64_t raw_ms,
                                     int64_t &canonical_ms);

}  // namespace aircannect
