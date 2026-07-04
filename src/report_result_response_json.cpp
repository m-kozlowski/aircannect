#include "report_manager.h"

#include <algorithm>

#include "json_util.h"
#include "report_result_json.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

const char *report_provider_id_name(ReportProviderId provider) {
    switch (provider) {
        case ReportProviderId::Edf: return "edf";
        case ReportProviderId::Spool: return "spool";
        default: return "unknown";
    }
}

}  // namespace

void ReportManager::build_result_json_from(
    const ReportResultStatus &result_status_,
    const ReportIndexedNight &indexed_night,
    const PlotRange *ranges,
    size_t range_count,
    const ReportResultStream *result_streams_,
    size_t result_stream_count_,
    const ReportCacheFetchStatus &cache_status_,
    LargeTextBuffer &json) const {
    const ReportSummaryRecord &result_night_ = indexed_night.summary;

    json.clear();
    json += "{";
    json_add_string(json,
                    "state",
                    report_result_state_name(result_status_.state),
                    false);
    json_add_string(json, "error", result_status_.error.c_str());
    json_add_int(json,
                 "therapy_index",
                 static_cast<long>(result_status_.therapy_index));
    json_add_uint64(json, "start", result_status_.night_start_ms);
    json_add_uint64(json, "end", result_status_.night_end_ms);
    json_add_int(json,
                 "duration_min",
                 static_cast<long>(result_status_.duration_min));
    json_add_int(json,
                 "missing_required",
                 static_cast<long>(result_status_.missing_required));
    json_add_int(json,
                 "missing_streams",
                 static_cast<long>(result_status_.missing_streams));
    json_add_int(json,
                 "streams",
                 static_cast<long>(result_status_.stream_count));
    json_add_int(json,
                 "chunks",
                 static_cast<long>(result_status_.chunk_count));
    json_add_int(json,
                 "records",
                 static_cast<long>(result_status_.record_count));
    json_add_int(json,
                 "bytes",
                 static_cast<long>(result_status_.payload_bytes));

    report_append_result_cache_json(json, cache_status_);
    report_append_result_metrics_json(json, result_status_);

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
                   : result_night_.session_interval_count);

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
            const PlotRange &range = ranges[i];
            session_start = static_cast<uint64_t>(range.start_ms);
            session_end = static_cast<uint64_t>(range.end_ms);
            duration_min =
                report_ceil_duration_min(range.start_ms, range.end_ms);
        } else {
            const ReportSummarySession &session = result_night_.sessions[i];
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
                                             result_streams_,
                                             result_stream_count_);
    json += "]}";
}

void ReportManager::build_result_chunks_json(LargeTextBuffer &json,
                                             size_t offset,
                                             size_t limit) const {
    if (limit > 128) limit = 128;
    const size_t total = result_status_.chunk_count;
    if (offset > total) offset = total;
    const size_t end = std::min(total, offset + limit);

    json.clear();
    json += "{";
    json_add_string(json, "state", result_state_name(), false);
    json_add_int(json, "offset", static_cast<long>(offset));
    json_add_int(json, "limit", static_cast<long>(limit));
    json_add_int(json, "total", static_cast<long>(total));
    json_add_bool(json, "more", end < total);
    json += ",\"chunks\":[";

    for (size_t i = offset; i < end; ++i) {
        const ReportResultChunk &chunk = result_chunks_[i];
        if (i != offset) json += ',';

        json += "{";
        json_add_int(json, "id", static_cast<long>(i), false);
        json_add_string(json, "kind", ReportStore::kind_name(chunk.kind));
        json_add_string(json,
                        "source",
                        report_source_spool_type(chunk.source));
        json_add_string(json,
                        "provider",
                        report_provider_id_name(
                            chunk.provider_ref.provider));
        json_add_string(json, "name", chunk.name ? chunk.name : "");
        json_add_int(json,
                     "stream",
                     static_cast<long>(chunk.stream_index));
        json_add_uint64(json,
                        "start",
                        static_cast<uint64_t>(chunk.start_ms));
        json_add_uint64(json, "end", static_cast<uint64_t>(chunk.end_ms));
        json_add_int(json,
                     "schema",
                     static_cast<long>(chunk.payload_schema));
        json_add_int(json,
                     "records",
                     static_cast<long>(chunk.record_count));
        json_add_int(json,
                     "bytes",
                     static_cast<long>(chunk.payload_len));
        json += "}";
    }

    json += "]}";
}

}  // namespace aircannect
