#include "report_summary_json.h"

#include <algorithm>
#include <stdio.h>

#include "json_util.h"
#include "spool_client.h"

namespace aircannect {
namespace {

void append_u64(LargeTextBuffer &out, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    out += buf;
}

void append_long(LargeTextBuffer &out, long value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", value);
    out += buf;
}

void append_optional_float(LargeTextBuffer &json,
                           const char *key,
                           bool present,
                           float value) {
    if (present) json_add_float(json, key, value);
}

void append_summary_sessions_json(LargeTextBuffer &json,
                                  const ReportIndexedNight &night) {
    const size_t range_count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
    bool first = true;

    if (range_count > 0) {
        for (size_t i = 0; i < range_count; ++i) {
            const ReportSessionRange &range = night.ranges[i];
            if (range.end_ms <= range.start_ms) continue;

            if (!first) json += ',';
            first = false;

            json += "{";
            json_add_uint64(json,
                            "start",
                            static_cast<uint64_t>(range.start_ms),
                            false);
            json_add_int(json,
                         "duration_min",
                         static_cast<long>(report_ceil_duration_min(
                             range.start_ms,
                             range.end_ms)));
            json += "}";
        }
        return;
    }

    const ReportSummaryRecord &record = night.summary;
    for (uint32_t session_index = 0;
         session_index < record.session_interval_count;
         ++session_index) {
        const ReportSummarySession &session = record.sessions[session_index];

        if (!first) json += ',';
        first = false;

        json += "{";
        json_add_uint64(json, "start", session.start_ms, false);
        json_add_int(json,
                     "duration_min",
                     static_cast<long>(session.duration_min));
        json += "}";
    }
}

}  // namespace

const char *report_summary_state_name(ReportSummaryState state) {
    switch (state) {
        case ReportSummaryState::Idle: return "idle";
        case ReportSummaryState::Fetching: return "fetching";
        case ReportSummaryState::Ready: return "ready";
        case ReportSummaryState::Error: return "error";
    }
    return "unknown";
}

uint32_t report_indexed_night_display_duration_min(
    const ReportIndexedNight &night) {
    uint32_t duration_min = 0;
    const size_t range_count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));

    for (size_t i = 0; i < range_count; ++i) {
        duration_min += report_ceil_duration_min(night.ranges[i].start_ms,
                                                 night.ranges[i].end_ms);
    }

    if (duration_min > 0) return duration_min;
    return night.summary.duration_min;
}

bool report_indexed_night_visible_in_summary(
    const ReportIndexedNight &night) {
    return night.summary.valid && night.range_count > 0 &&
           report_indexed_night_display_duration_min(night) > 0;
}

void report_append_summary_json_from_indexed(
    LargeTextBuffer &json,
    const ReportSummaryStatus &status,
    uint32_t data_epoch,
    const ReportIndexedNight *nights,
    size_t night_count) {
    json.clear();
    if (!nights) night_count = 0;

    json += "{";
    json_add_string(json,
                    "state",
                    report_summary_state_name(status.state),
                    false);
    json_add_int(json, "revision", static_cast<long>(status.revision));
    json_add_int(json, "data_epoch", static_cast<long>(data_epoch));
    json_add_string(json, "error", status.error.c_str());
    json_add_int(json, "records_total",
                 static_cast<long>(status.records_total));
    json_add_int(json, "nights_with_therapy",
                 static_cast<long>(status.nights_with_therapy));
    json_add_int(json, "elapsed_ms", static_cast<long>(status.elapsed_ms));
    json_add_string(json, "active_spool", status.active_spool.c_str());

    json += ",\"spool\":{\"state\":\"";
    json += spool_client_state_name(status.spool.state);
    json += "\",\"round\":";
    append_long(json, static_cast<long>(status.spool.current_round));
    json += ",\"fragments\":";
    append_long(json, static_cast<long>(status.spool.fragments));
    json += ",\"bytes\":";
    append_long(json, static_cast<long>(status.spool.bytes));
    json += "},\"nights\":[";

    bool first = true;
    size_t therapy_seen = 0;
    for (size_t i = 0; i < night_count; ++i) {
        const ReportIndexedNight &night = nights[i];
        const ReportSummaryRecord &record = night.summary;
        const uint32_t duration_min =
            report_indexed_night_display_duration_min(night);

        if (!report_indexed_night_visible_in_summary(night)) continue;

        if (!first) json += ',';
        first = false;

        const size_t therapy_index =
            status.nights_with_therapy > therapy_seen
                ? status.nights_with_therapy - therapy_seen - 1
                : 0;
        therapy_seen++;

        json += "{";
        json_add_int(json, "index", static_cast<long>(i), false);
        json_add_int(json, "therapy_index",
                     static_cast<long>(therapy_index));
        json_add_uint64(json, "start", record.start_ms);
        json_add_uint64(json, "end", record.end_ms);
        json_add_int(json, "duration_min",
                     static_cast<long>(duration_min));
        if (record.has_tz_offset_min) {
            json_add_int(json, "tz_offset_min",
                         static_cast<long>(record.tz_offset_min));
        }

        append_optional_float(json, "ahi", record.has_ahi, record.ahi);
        append_optional_float(json, "apnea_index",
                              record.has_apnea_index,
                              record.apnea_index);
        append_optional_float(json, "hypopnea_index",
                              record.has_hypopnea_index,
                              record.hypopnea_index);
        append_optional_float(json, "oa_index",
                              record.has_oa_index,
                              record.oa_index);
        append_optional_float(json, "ca_index",
                              record.has_ca_index,
                              record.ca_index);
        append_optional_float(json, "ua_index",
                              record.has_ua_index,
                              record.ua_index);
        append_optional_float(json, "rera_index",
                              record.has_rera_index,
                              record.rera_index);

        json_add_int(json,
                     "session_count",
                     static_cast<long>(night.range_count > 0
                                           ? night.range_count
                                           : record.session_count));
        json += ",\"sessions\":[";
        append_summary_sessions_json(json, night);
        json += "]";
        json_add_int(json, "has_summary", night.has_summary ? 1 : 0);
        json_add_int(json, "has_edf", night.has_edf ? 1 : 0);
        json += ",\"id\":\"";
        append_u64(json, record.start_ms);
        json += "\"}";
    }

    json += "]}";
}

}  // namespace aircannect
