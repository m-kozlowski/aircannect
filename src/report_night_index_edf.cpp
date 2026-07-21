#include "report_night_index_internal.h"

#include <algorithm>
#include <string.h>

#include "calendar_utils.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr int64_t SUMMARY_SESSION_EDGE_TOLERANCE_MS = 2LL * 60LL * 1000LL;

bool sleep_day_start_utc_ms(const char *sleep_day,
                            int32_t timezone_offset_minutes,
                            int64_t &out_ms) {
    int64_t days = 0;
    if (!calendar_yyyymmdd_to_days(sleep_day, days)) return false;
    out_ms = days * REPORT_DAY_MS + REPORT_NOON_MS -
             static_cast<int64_t>(timezone_offset_minutes) * 60LL * 1000LL;
    return true;
}

bool append_edf_source_signature(ReportIndexedNight &night,
                                 uint64_t signature) {
    size_t count = std::min(
        night.edf_source_signature_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    for (size_t i = 0; i < count; ++i) {
        if (night.edf_source_signatures[i] == signature) return true;
    }
    if (count >= AC_REPORT_EDF_SESSION_MAX) return false;

    night.edf_source_signatures[count] = signature;
    night.edf_source_signature_count = count + 1;
    return true;
}

void sync_summary_sessions_from_ranges(ReportIndexedNight &night) {
    uint32_t duration_sum = 0;
    for (uint32_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        night.summary.sessions[i] = ReportSummarySession{};
    }

    const uint32_t stored_session_count =
        static_cast<uint32_t>(std::min<size_t>(
            night.range_count,
            AC_REPORT_SUMMARY_SESSION_MAX));
    for (uint32_t i = 0; i < stored_session_count; ++i) {
        night.summary.sessions[i].start_ms =
            static_cast<uint64_t>(night.ranges[i].start_ms);
        night.summary.sessions[i].duration_min =
            report_ceil_duration_min(night.ranges[i].start_ms,
                                     night.ranges[i].end_ms);
    }
    for (size_t i = 0; i < night.range_count; ++i) {
        duration_sum += report_ceil_duration_min(night.ranges[i].start_ms,
                                                 night.ranges[i].end_ms);
    }

    night.summary.session_interval_count = stored_session_count;
    night.summary.session_count = static_cast<uint32_t>(night.range_count);
    night.summary.has_session_count = night.range_count > 0;
    if (night.range_count > 0) night.summary.duration_min = duration_sum;
}

void clear_summary_aggregate_metrics(ReportSummaryRecord &summary) {
    summary.has_ahi = false;
    summary.ahi = 0.0f;
    summary.has_apnea_index = false;
    summary.apnea_index = 0.0f;
    summary.has_hypopnea_index = false;
    summary.hypopnea_index = 0.0f;
    summary.has_oa_index = false;
    summary.oa_index = 0.0f;
    summary.has_ca_index = false;
    summary.ca_index = 0.0f;
    summary.has_ua_index = false;
    summary.ua_index = 0.0f;
    summary.has_rera_index = false;
    summary.rera_index = 0.0f;
    summary.summary_field_mask = 0;
    for (size_t i = 0; i < AC_REPORT_SUMMARY_FIELD_COUNT; ++i) {
        summary.summary_field_values[i] = 0;
    }
}

bool timestamps_within(int64_t lhs, int64_t rhs, int64_t tolerance_ms) {
    if (lhs >= rhs) return lhs - rhs <= tolerance_ms;
    return rhs - lhs <= tolerance_ms;
}

bool raw_time_from_canonical(int64_t canonical_ms,
                             int64_t device_minus_utc_ms,
                             int64_t &raw_ms) {
    if (device_minus_utc_ms > 0 &&
        canonical_ms > INT64_MAX - device_minus_utc_ms) {
        return false;
    }
    if (device_minus_utc_ms < 0 &&
        canonical_ms < INT64_MIN - device_minus_utc_ms) {
        return false;
    }

    raw_ms = canonical_ms + device_minus_utc_ms;
    return true;
}

int find_indexed_night_by_sleep_day(ReportIndexedNight *nights,
                                    size_t count,
                                    const char *sleep_day) {
    if (!nights || !sleep_day || !sleep_day[0]) return -1;

    for (size_t i = 0; i < count; ++i) {
        char indexed_sleep_day[9] = {};
        if (report_summary_sleep_day_yyyymmdd(nights[i].summary,
                                              indexed_sleep_day,
                                              sizeof(indexed_sleep_day)) &&
            strcmp(indexed_sleep_day, sleep_day) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool merge_range_into_indexed_night(ReportIndexedNight &night,
                                    int64_t start_ms,
                                    int64_t end_ms) {
    if (end_ms <= start_ms) return false;
    size_t count = std::min(night.range_count,
                            static_cast<size_t>(
                                AC_REPORT_NIGHT_SESSION_MAX));

    int best = -1;
    for (size_t i = 0; i < count; ++i) {
        const ReportSessionRange &existing = night.ranges[i];
        if (existing.end_ms <= existing.start_ms) continue;
        if (ranges_overlap(start_ms,
                           end_ms,
                           existing.start_ms,
                           existing.end_ms)) {
            best = static_cast<int>(i);
            break;
        }
    }

    if (best >= 0) {
        ReportSessionRange &target =
            night.ranges[static_cast<size_t>(best)];
        target.start_ms = std::min(target.start_ms, start_ms);
        target.end_ms = std::max(target.end_ms, end_ms);

        normalize_range_array(night.ranges, night.range_count);
        sync_summary_sessions_from_ranges(night);
        return true;
    }

    if (count >= AC_REPORT_NIGHT_SESSION_MAX) return false;

    night.ranges[count].start_ms = start_ms;
    night.ranges[count].end_ms = end_ms;
    night.range_count = count + 1;

    normalize_range_array(night.ranges, night.range_count);
    sync_summary_sessions_from_ranges(night);
    return true;
}

bool merge_data_range_into_indexed_night(ReportIndexedNight &night,
                                         int64_t start_ms,
                                         int64_t end_ms) {
    if (end_ms <= start_ms) return false;
    size_t count = std::min(night.data_range_count,
                            static_cast<size_t>(
                                AC_REPORT_NIGHT_SESSION_MAX));

    for (size_t i = 0; i < count; ++i) {
        ReportSessionRange &existing = night.data_ranges[i];
        if (existing.end_ms <= existing.start_ms) continue;

        if (ranges_overlap(start_ms,
                           end_ms,
                           existing.start_ms,
                           existing.end_ms) ||
            start_ms == existing.end_ms || end_ms == existing.start_ms) {
            existing.start_ms = std::min(existing.start_ms, start_ms);
            existing.end_ms = std::max(existing.end_ms, end_ms);

            normalize_range_array(night.data_ranges, night.data_range_count);
            coalesce_sorted_range_array(night.data_ranges,
                                        night.data_range_count);
            return true;
        }
    }

    if (count >= AC_REPORT_NIGHT_SESSION_MAX) return false;

    night.data_ranges[count].start_ms = start_ms;
    night.data_ranges[count].end_ms = end_ms;
    night.data_range_count = count + 1;

    normalize_range_array(night.data_ranges, night.data_range_count);
    coalesce_sorted_range_array(night.data_ranges, night.data_range_count);
    return true;
}

int find_matching_indexed_range(const ReportIndexedNight &night,
                                int64_t start_ms,
                                int64_t end_ms) {
    if (end_ms <= start_ms) return -1;

    const size_t count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
    int best = -1;
    int64_t best_gap = INT64_MAX;

    for (size_t i = 0; i < count; ++i) {
        const ReportSessionRange &existing = night.ranges[i];
        if (existing.end_ms <= existing.start_ms) continue;

        int64_t gap = 0;
        if (ranges_overlap(start_ms,
                           end_ms,
                           existing.start_ms,
                           existing.end_ms)) {
            gap = 0;
        } else if (end_ms <= existing.start_ms) {
            gap = existing.start_ms - end_ms;
        } else {
            gap = start_ms - existing.end_ms;
        }

        if (gap <= REPORT_SESSION_MERGE_TOLERANCE_MS && gap < best_gap) {
            best = static_cast<int>(i);
            best_gap = gap;
        }
    }

    return best;
}

int find_indexed_night_for_edf_session(const ReportIndexedNight *nights,
                                       size_t count,
                                       const char *sleep_day,
                                       bool have_day_start,
                                       int64_t day_start_ms,
                                       int64_t session_start_ms,
                                       int64_t session_end_ms) {
    if (!nights) return -1;

    if (sleep_day && sleep_day[0]) {
        for (size_t i = 0; i < count; ++i) {
            char indexed_sleep_day[9] = {};
            if (report_summary_sleep_day_yyyymmdd(nights[i].summary,
                                                  indexed_sleep_day,
                                                  sizeof(indexed_sleep_day)) &&
                strcmp(indexed_sleep_day, sleep_day) == 0) {
                return static_cast<int>(i);
            }
        }
    }

    if (have_day_start) {
        for (size_t i = 0; i < count; ++i) {
            const ReportSummaryRecord &record = nights[i].summary;
            if (!record.valid) continue;
            if (static_cast<int64_t>(record.start_ms) == day_start_ms) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        const ReportSummaryRecord &record = nights[i].summary;
        if (!record.valid || record.end_ms <= record.start_ms) {
            continue;
        }
        if (ranges_overlap(static_cast<int64_t>(record.start_ms),
                           static_cast<int64_t>(record.end_ms),
                           session_start_ms,
                           session_end_ms)) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

}  // namespace

bool ReportNightIndex::suppress_raw_summary_for_edf(
    const EdfReportSessionDescriptor &session) {
    if (!nights_ || !session.clock_provenance_present) return true;

    const EdfSessionMetadata &metadata = session.clock_provenance;
    char raw_sleep_day[9] = {};
    if (session.clock_provenance_decoded) {
        if (!metadata.raw_sleep_day.format_yyyymmdd(raw_sleep_day,
                                                    sizeof(raw_sleep_day))) {
            return false;
        }
    } else {
        copy_cstr(raw_sleep_day, sizeof(raw_sleep_day), session.sleep_day);
    }

    const int raw_index = find_indexed_night_by_sleep_day(nights_,
                                                           count_,
                                                           raw_sleep_day);
    if (raw_index < 0) return true;

    ReportIndexedNight &night = nights_[static_cast<size_t>(raw_index)];
    if (!night.has_summary) return true;

    const bool same_sleep_day =
        !session.clock_provenance_decoded ||
        metadata.raw_sleep_day == metadata.canonical_sleep_day;
    if (same_sleep_day) {
        if (!night.has_edf) {
            for (size_t i = static_cast<size_t>(raw_index) + 1;
                 i < count_;
                 ++i) {
                nights_[i - 1] = nights_[i];
            }
            count_--;
            nights_[count_] = ReportIndexedNight{};
            return true;
        }

        night.has_summary = false;
        clear_summary_aggregate_metrics(night.summary);
        return true;
    }

    int64_t raw_start = metadata.raw_segment_start_ms;
    int64_t raw_end = metadata.raw_segment_end_ms;
    if (metadata.finalized &&
        metadata.raw_therapy_start_ms > 0 &&
        metadata.raw_therapy_end_ms > metadata.raw_therapy_start_ms) {
        raw_start = metadata.raw_therapy_start_ms;
        raw_end = metadata.raw_therapy_end_ms;
    } else if (raw_end <= raw_start &&
               !raw_time_from_canonical(session.latest_header_end_ms,
                                        metadata.device_minus_utc_ms,
                                        raw_end)) {
        return false;
    }
    if (raw_start <= 0 || raw_end <= raw_start) return true;

    size_t write = 0;
    bool matched = false;
    for (size_t i = 0; i < night.range_count; ++i) {
        const ReportSessionRange &range = night.ranges[i];
        const bool is_match =
            timestamps_within(range.start_ms,
                              raw_start,
                              SUMMARY_SESSION_EDGE_TOLERANCE_MS) &&
            timestamps_within(range.end_ms,
                              raw_end,
                              SUMMARY_SESSION_EDGE_TOLERANCE_MS);
        if (is_match) {
            matched = true;
            continue;
        }
        night.ranges[write++] = range;
    }
    if (!matched) return true;

    for (size_t i = write; i < AC_REPORT_NIGHT_SESSION_MAX; ++i) {
        night.ranges[i] = ReportSessionRange{};
    }
    night.range_count = write;

    if (write == 0 && !night.has_edf) {
        for (size_t i = static_cast<size_t>(raw_index) + 1;
             i < count_;
             ++i) {
            nights_[i - 1] = nights_[i];
        }
        count_--;
        nights_[count_] = ReportIndexedNight{};
        return true;
    }

    sync_summary_sessions_from_ranges(night);
    clear_summary_aggregate_metrics(night.summary);
    if (write == 0) night.has_summary = false;
    return true;
}

bool ReportNightIndex::add_edf_session(
    const EdfReportSessionDescriptor &session,
    bool timezone_offset_valid,
    int32_t timezone_offset_minutes) {
    if (!nights_) return false;

    const bool has_numeric = edf_session_has_report_numeric(session);
    const bool has_event = edf_session_has_report_annotation(session);
    if (!has_numeric && !has_event) return true;

    int64_t day_start_ms = 0;
    const bool have_day_start =
        timezone_offset_valid &&
        sleep_day_start_utc_ms(session.sleep_day,
                               timezone_offset_minutes,
                               day_start_ms);
    const int64_t session_start_ms = session.earliest_header_start_ms;
    const int64_t session_end_ms = session.latest_header_end_ms;
    int index = find_indexed_night_for_edf_session(nights_,
                                                   count_,
                                                   session.sleep_day,
                                                   have_day_start,
                                                   day_start_ms,
                                                   session_start_ms,
                                                   session_end_ms);

    if (index < 0) {
        if (!has_numeric || !have_day_start) return true;
        if (count_ >= capacity_) return false;

        ReportIndexedNight &night = nights_[count_];
        night = ReportIndexedNight{};

        ReportSummaryRecord &record = night.summary;
        record.valid = true;
        record.start_ms = static_cast<uint64_t>(day_start_ms);
        record.end_ms = static_cast<uint64_t>(day_start_ms + REPORT_DAY_MS);
        record.has_tz_offset_min = true;
        record.tz_offset_min = timezone_offset_minutes;
        index = static_cast<int>(count_++);
    }

    ReportIndexedNight &night = nights_[static_cast<size_t>(index)];
    ReportSummaryRecord &record = night.summary;
    if (timezone_offset_valid && !record.has_tz_offset_min) {
        record.has_tz_offset_min = true;
        record.tz_offset_min = timezone_offset_minutes;
    }

    bool accepted = false;
    if (has_numeric) {
        const bool matched_existing_summary_range =
            night.has_summary &&
            find_matching_indexed_range(night,
                                        session_start_ms,
                                        session_end_ms) >= 0;
        const bool data_range_added =
            merge_data_range_into_indexed_night(night,
                                                session_start_ms,
                                                session_end_ms);
        const bool display_range_ok =
            matched_existing_summary_range ||
            merge_range_into_indexed_night(night,
                                           session_start_ms,
                                           session_end_ms);
        accepted = data_range_added && display_range_ok;
    } else {
        accepted = true;
    }

    if (!accepted) return false;
    if (!append_edf_source_signature(
            night,
            report_edf_session_signature(session))) {
        return false;
    }

    night.has_edf = true;
    night.has_edf_clock_provenance =
        night.has_edf_clock_provenance ||
        session.clock_provenance_present;
    night.edf_catalog_pending = false;
    return true;
}

}  // namespace aircannect
