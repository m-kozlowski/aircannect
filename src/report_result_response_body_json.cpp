#include "report_result_response_json.h"

#include <algorithm>

#include "json_util.h"
#include "report_result_json.h"

namespace aircannect {

void build_report_result_json_from(
    const ReportResultStatus &result_status,
    const ReportIndexedNight &indexed_night,
    const report_manager_internal::PlotRange *ranges,
    size_t range_count,
    const ReportResultStream *result_streams,
    size_t result_stream_count,
    const ReportCacheFetchStatus &cache_status,
    LargeTextBuffer &json) {
    const ReportSummaryRecord &result_night = indexed_night.summary;

    json.clear();
    json += "{";
    json_add_string(json,
                    "state",
                    report_result_state_name(result_status.state),
                    false);
    json_add_string(json, "error", result_status.error.c_str());
    json_add_int(json,
                 "therapy_index",
                 static_cast<long>(result_status.therapy_index));
    json_add_uint64(json, "start", result_status.night_start_ms);
    json_add_uint64(json, "end", result_status.night_end_ms);
    json_add_int(json,
                 "duration_min",
                 static_cast<long>(result_status.duration_min));
    json_add_int(json,
                 "missing_required",
                 static_cast<long>(result_status.missing_required));
    json_add_int(json,
                 "missing_streams",
                 static_cast<long>(result_status.missing_streams));
    json_add_int(json,
                 "streams",
                 static_cast<long>(result_status.stream_count));
    json_add_int(json,
                 "chunks",
                 static_cast<long>(result_status.chunk_count));
    json_add_int(json,
                 "records",
                 static_cast<long>(result_status.record_count));
    json_add_int(json,
                 "bytes",
                 static_cast<long>(result_status.payload_bytes));

    report_append_result_cache_json(json, cache_status);
    report_append_result_metrics_json(json, result_status);

    json += ",\"sessions\":[";
    const bool have_index_ranges = indexed_night.range_count > 0;
    const bool have_resolved_ranges = !have_index_ranges &&
                                      ranges &&
                                      range_count > 0;
    const size_t session_json_count =
        have_index_ranges
            ? std::min(indexed_night.range_count,
                       static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX))
            : (have_resolved_ranges
                   ? range_count
                   : result_night.session_interval_count);

    for (size_t i = 0; i < session_json_count; ++i) {
        uint64_t session_start = 0;
        uint64_t session_end = 0;
        uint32_t duration_min = 0;

        if (have_index_ranges) {
            const ReportSessionRange &range = indexed_night.ranges[i];
            session_start = static_cast<uint64_t>(range.start_ms);
            session_end = static_cast<uint64_t>(range.end_ms);
            duration_min =
                report_ceil_duration_min(range.start_ms, range.end_ms);
        } else if (have_resolved_ranges) {
            const report_manager_internal::PlotRange &range = ranges[i];
            session_start = static_cast<uint64_t>(range.start_ms);
            session_end = static_cast<uint64_t>(range.end_ms);
            duration_min =
                report_ceil_duration_min(range.start_ms, range.end_ms);
        } else {
            const ReportSummarySession &session = result_night.sessions[i];
            session_start = session.start_ms;
            session_end = session.start_ms +
                          static_cast<uint64_t>(session.duration_min) *
                              60000ULL;
            duration_min = session.duration_min;
        }

        if (i) json += ',';
        json += "{";
        json_add_uint64(json, "start", session_start, false);
        json_add_uint64(json, "end", session_end);
        json_add_int(json, "duration_min", static_cast<long>(duration_min));
        json += "}";
    }
    json += "]";

    report_append_result_stream_details_json(json,
                                             result_streams,
                                             result_stream_count);
    json += "}";
}

}  // namespace aircannect
