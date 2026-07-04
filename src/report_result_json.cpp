#include "report_result_json.h"

#include "json_util.h"
#include "report_daily_metrics.h"
#include "report_sources.h"
#include "spool_client.h"

namespace aircannect {
namespace {

void append_optional_float(LargeTextBuffer &json,
                           const char *key,
                           bool present,
                           float value) {
    if (present) json_add_float(json, key, value);
}

const char *report_stream_provider_name(const ReportResultStream &stream) {
    if (stream.has_edf_segment && stream.has_spool_segment) return "mixed";
    if (stream.has_edf_segment) return "edf";
    if (stream.has_spool_segment) return "spool";
    return "none";
}

bool report_stream_low_res(const ReportResultStream &stream) {
    if (stream.kind != ReportStoreChunkKind::Series) return false;

    size_t signal_def_count = 0;
    const ReportSignalDef *signal_defs = report_signal_defs(signal_def_count);
    for (size_t i = 0; i < signal_def_count; ++i) {
        if (signal_defs[i].id != stream.signal) continue;
        return stream.source != signal_defs[i].preferred_source;
    }
    return false;
}

}  // namespace

const char *report_result_state_name(ReportResultState state) {
    switch (state) {
        case ReportResultState::Idle: return "idle";
        case ReportResultState::Preparing: return "preparing";
        case ReportResultState::Ready: return "ready";
        case ReportResultState::Incomplete: return "incomplete";
        case ReportResultState::Partial: return "partial";
        case ReportResultState::Error: return "error";
    }
    return "unknown";
}

void report_append_result_cache_json(
    LargeTextBuffer &json,
    const ReportCacheFetchStatus &cache_status) {
    json += ",\"cache\":{";
    json_add_bool(json, "active", cache_status.active, false);
    json_add_int(json, "revision",
                 static_cast<long>(cache_status.revision));
    json_add_string(json,
                    "source",
                    report_source_spool_type(cache_status.active_source));
    json_add_int(json,
                 "source_index",
                 static_cast<long>(cache_status.source_index));
    json_add_int(json,
                 "source_count",
                 static_cast<long>(cache_status.source_count));
    json_add_int(json,
                 "chunks_written",
                 static_cast<long>(cache_status.chunks_written));
    json_add_string(json, "error", cache_status.error.c_str());
    json_add_string(json,
                    "spool_state",
                    spool_client_state_name(cache_status.spool.state));
    json_add_int(json,
                 "spool_round",
                 static_cast<long>(cache_status.spool.current_round));
    json_add_int(json,
                 "spool_fragments",
                 static_cast<long>(cache_status.spool.fragments));
    json_add_int(json,
                 "spool_bytes",
                 static_cast<long>(cache_status.spool.bytes));
    json_add_int(json,
                 "spool_elapsed_ms",
                 static_cast<long>(cache_status.spool.elapsed_ms));
    json += "}";
}

void report_append_result_metrics_json(
    LargeTextBuffer &json,
    const ReportResultStatus &status) {
    append_optional_float(json, "ahi", status.ahi_valid, status.ahi);
    if (status.ahi_valid) {
        json_add_string(json, "ahi_source",
                        report_metric_source_name(status.ahi_source));
    }

    append_optional_float(json, "oa_index",
                          status.oa_index_valid,
                          status.oa_index);
    append_optional_float(json, "ca_index",
                          status.ca_index_valid,
                          status.ca_index);
    append_optional_float(json, "ua_index",
                          status.ua_index_valid,
                          status.ua_index);
    append_optional_float(json, "hypopnea_index",
                          status.hypopnea_index_valid,
                          status.hypopnea_index);
    append_optional_float(json, "arousal_index",
                          status.arousal_index_valid,
                          status.arousal_index);

    append_optional_float(json,
                          "mask_pressure_50",
                          status.mask_pressure_50_valid,
                          status.mask_pressure_50_cm_h2o);
    if (status.mask_pressure_50_valid) {
        json_add_string(
            json,
            "mask_pressure_50_source",
            report_metric_source_name(status.mask_pressure_50_source));
    }

    append_optional_float(json,
                          "average_pressure",
                          status.mask_pressure_50_valid,
                          status.mask_pressure_50_cm_h2o);
    if (status.mask_pressure_50_valid) {
        json_add_string(
            json,
            "average_pressure_source",
            report_metric_source_name(status.mask_pressure_50_source));
    }

    append_optional_float(json, "leak_50",
                          status.leak_50_valid,
                          status.leak_50_l_min);
    if (status.leak_50_valid) {
        json_add_string(json, "leak_50_source",
                        report_metric_source_name(status.leak_50_source));
    }

    append_optional_float(json, "average_leak",
                          status.leak_50_valid,
                          status.leak_50_l_min);
    if (status.leak_50_valid) {
        json_add_string(json, "average_leak_source",
                        report_metric_source_name(status.leak_50_source));
    }

    json_add_int(json, "oa_count",
                 static_cast<long>(status.oa_count));
    json_add_int(json, "ca_count",
                 static_cast<long>(status.ca_count));
    json_add_int(json, "ua_count",
                 static_cast<long>(status.ua_count));
    json_add_int(json, "hypopnea_count",
                 static_cast<long>(status.hypopnea_count));
    json_add_int(json, "arousal_count",
                 static_cast<long>(status.arousal_count));
    json_add_bool(json, "events_available", status.events_available);
}

void report_append_result_stream_details_json(
    LargeTextBuffer &json,
    const ReportResultStream *streams,
    size_t stream_count) {
    json += ",\"stream_details\":[";
    for (size_t i = 0; i < stream_count; ++i) {
        const ReportResultStream &stream = streams[i];
        if (i) json += ',';

        json += "{";
        json_add_string(json,
                        "kind",
                        ReportStore::kind_name(stream.kind),
                        false);
        json_add_string(json,
                        "source",
                        report_source_spool_type(stream.source));
        json_add_string(json, "name", stream.name ? stream.name : "");
        json_add_bool(json, "required", stream.required);
        json_add_bool(json, "complete", stream.complete);
        json_add_string(json, "provider", report_stream_provider_name(stream));
        json_add_bool(json, "has_edf", stream.has_edf_segment);
        json_add_bool(json, "has_spool", stream.has_spool_segment);
        json_add_int(json, "chunks",
                     static_cast<long>(stream.chunk_count));
        json_add_int(json, "records",
                     static_cast<long>(stream.record_count));
        json_add_int(json, "bytes",
                     static_cast<long>(stream.payload_bytes));
        json_add_bool(json, "low_res", report_stream_low_res(stream));
        json += "}";
    }
    json += "]";
}

}  // namespace aircannect
