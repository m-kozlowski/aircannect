#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "edf_report_session.h"
#include "report_proto.h"

namespace aircannect {

// Summary RPC records carry at most 16 intervals, but EDF-backed nights can
// contain many more short logical sessions. Keep the report model independent
// from the Summary wire-format limit.
static constexpr size_t AC_REPORT_EDF_SESSION_MAX =
    AC_REPORT_SUMMARY_SESSION_MAX * 4;
static constexpr size_t AC_REPORT_NIGHT_SESSION_MAX =
    AC_REPORT_EDF_SESSION_MAX + AC_REPORT_SUMMARY_SESSION_MAX;

struct ReportSessionRange {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

struct ReportIndexedNight {
    ReportSummaryRecord summary;
    ReportSessionRange ranges[AC_REPORT_NIGHT_SESSION_MAX] = {};
    size_t range_count = 0;
    ReportSessionRange data_ranges[AC_REPORT_NIGHT_SESSION_MAX] = {};
    size_t data_range_count = 0;
    uint64_t edf_source_signatures[AC_REPORT_EDF_SESSION_MAX] = {};
    size_t edf_source_signature_count = 0;
    uint64_t source_signature = 0;
    bool has_summary = false;
    bool has_edf = false;
    bool edf_catalog_pending = false;
};

bool ranges_overlap(int64_t start_a,
                    int64_t end_a,
                    int64_t start_b,
                    int64_t end_b);
size_t collect_session_ranges(const ReportSummaryRecord &night,
                              ReportSessionRange *ranges,
                              size_t max_ranges);
bool night_data_span(const ReportSummaryRecord &night,
                     int64_t &span_start,
                     int64_t &span_end);
bool indexed_night_data_span(const ReportIndexedNight &night,
                             int64_t &span_start,
                             int64_t &span_end);
bool indexed_night_summary_ranges_covered_by_data(
    const ReportIndexedNight &night);
void normalize_report_indexed_night(ReportIndexedNight &night);
size_t collect_indexed_night_data_ranges(const ReportIndexedNight &night,
                                         ReportSessionRange *ranges,
                                         size_t max_ranges);
size_t collect_indexed_night_report_ranges(const ReportIndexedNight &night,
                                           ReportSessionRange *ranges,
                                           size_t max_ranges);
uint32_t report_ceil_duration_min(int64_t start_ms, int64_t end_ms);
uint64_t report_summary_identity_signature(
    const ReportSummaryRecord &record);
bool report_summary_sleep_day_yyyymmdd(const ReportSummaryRecord &record,
                                       char *out,
                                       size_t out_size);
class ReportNightIndex {
public:
    ReportNightIndex(ReportIndexedNight *nights, size_t capacity);

    void reset();
    bool add_indexed_night(const ReportIndexedNight &night);
    bool add_summary_record(const ReportSummaryRecord &record);
    bool add_edf_session(const EdfReportSessionDescriptor &session,
                         bool timezone_offset_valid,
                         int32_t timezone_offset_minutes);
    bool finish(ReportIndexedNight *sort_scratch);

    size_t count() const { return count_; }

    static bool by_therapy_index(const ReportIndexedNight *nights,
                                 size_t count,
                                 size_t therapy_index,
                                 ReportIndexedNight &out);
    static bool by_start(const ReportIndexedNight *nights,
                         size_t count,
                         uint64_t night_start_ms,
                         ReportIndexedNight &out,
                         size_t *therapy_index_out = nullptr);

private:
    ReportIndexedNight *nights_ = nullptr;
    size_t capacity_ = 0;
    size_t count_ = 0;
};

}  // namespace aircannect
