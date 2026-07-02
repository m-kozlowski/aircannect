#include "report_night_index.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar_utils.h"

namespace aircannect {
namespace {

constexpr int64_t REPORT_SESSION_MERGE_TOLERANCE_MS = 2 * 60 * 1000;
constexpr int64_t REPORT_SLEEP_DAY_MATCH_TOLERANCE_MS = 3 * 60 * 60 * 1000;
constexpr int64_t REPORT_DAY_MS = 24LL * 60LL * 60LL * 1000LL;
constexpr int64_t REPORT_NOON_MS = 12LL * 60LL * 60LL * 1000LL;
constexpr uint64_t REPORT_FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t REPORT_FNV_PRIME = 1099511628211ULL;

uint64_t report_hash_bytes(uint64_t hash, const void *data, size_t len) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= REPORT_FNV_PRIME;
    }
    return hash;
}

uint64_t report_hash_u64(uint64_t hash, uint64_t value) {
    return report_hash_bytes(hash, &value, sizeof(value));
}

uint64_t report_hash_i64(uint64_t hash, int64_t value) {
    return report_hash_bytes(hash, &value, sizeof(value));
}

uint64_t report_hash_u32(uint64_t hash, uint32_t value) {
    return report_hash_bytes(hash, &value, sizeof(value));
}

bool parse_sleep_day(const char *sleep_day,
                     int &year,
                     unsigned &month,
                     unsigned &day) {
    if (!sleep_day || strlen(sleep_day) != 8) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (sleep_day[i] < '0' || sleep_day[i] > '9') return false;
    }
    char buf[5] = {};
    memcpy(buf, sleep_day, 4);
    year = static_cast<int>(strtol(buf, nullptr, 10));
    buf[0] = sleep_day[4];
    buf[1] = sleep_day[5];
    buf[2] = '\0';
    month = static_cast<unsigned>(strtoul(buf, nullptr, 10));
    buf[0] = sleep_day[6];
    buf[1] = sleep_day[7];
    day = static_cast<unsigned>(strtoul(buf, nullptr, 10));
    return year > 0 &&
           month >= 1 && month <= 12 &&
           day >= 1 &&
           day <= calendar_days_in_month(year, static_cast<int>(month));
}

bool sleep_day_start_utc_ms(const char *sleep_day,
                            int32_t timezone_offset_minutes,
                            int64_t &out_ms) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!parse_sleep_day(sleep_day, year, month, day)) return false;
    const int64_t days = calendar_days_from_civil(year, month, day);
    out_ms = days * REPORT_DAY_MS + REPORT_NOON_MS -
             static_cast<int64_t>(timezone_offset_minutes) * 60LL * 1000LL;
    return true;
}

uint64_t summary_identity_signature(const ReportSummaryRecord &record) {
    uint64_t hash = REPORT_FNV_OFFSET;
    hash = report_hash_u64(hash, record.start_ms);
    hash = report_hash_u64(hash, record.end_ms);
    hash = report_hash_u32(hash, record.duration_min);
    hash = report_hash_u32(hash, record.session_interval_count);
    const uint32_t session_count = std::min<uint32_t>(
        record.session_interval_count, AC_REPORT_SUMMARY_SESSION_MAX);
    for (uint32_t i = 0; i < session_count; ++i) {
        hash = report_hash_u64(hash, record.sessions[i].start_ms);
        hash = report_hash_u32(hash, record.sessions[i].duration_min);
    }
    return hash;
}

uint64_t report_edf_session_signature(
    const EdfReportSessionDescriptor &session) {
    uint64_t hash = REPORT_FNV_OFFSET;
    hash = report_hash_bytes(hash, session.sleep_day, sizeof(session.sleep_day));
    hash = report_hash_bytes(hash,
                             session.session_stamp,
                             sizeof(session.session_stamp));
    hash = report_hash_u32(hash, session.file_mask);
    hash = report_hash_u32(hash, session.primary_signal_mask);
    hash = report_hash_u32(hash, session.fallback_signal_mask);
    hash = report_hash_u64(hash, session.total_size);
    hash = report_hash_u64(hash, static_cast<uint64_t>(session.latest_write));
    hash = report_hash_i64(hash, session.earliest_header_start_ms);
    hash = report_hash_i64(hash, session.latest_header_end_ms);
    return hash;
}

bool session_windows_match_with_tolerance(int64_t first_start_ms,
                                          int64_t first_end_ms,
                                          int64_t second_start_ms,
                                          int64_t second_end_ms,
                                          int64_t tolerance_ms) {
    if (first_start_ms <= 0 || second_start_ms <= 0) return false;
    if (first_end_ms <= first_start_ms) first_end_ms = first_start_ms;
    if (second_end_ms <= second_start_ms) second_end_ms = second_start_ms;
    if (first_start_ms <= second_end_ms + tolerance_ms &&
        second_start_ms <= first_end_ms + tolerance_ms) {
        return true;
    }
    return llabs(first_start_ms - second_start_ms) <= tolerance_ms;
}

bool edf_session_has_report_event_mask(
    const EdfReportSessionDescriptor &session) {
    const uint32_t event_mask =
        edf_report_file_kind_mask(EdfInventoryFileKind::Eve) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Csl);
    return (session.file_mask & event_mask) != 0;
}

void normalize_range_array(ReportSessionRange *ranges, size_t &count) {
    if (!ranges) {
        count = 0;
        return;
    }
    count = std::min(count,
                     static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    std::sort(ranges,
              ranges + count,
              [](const ReportSessionRange &a,
                 const ReportSessionRange &b) {
                  return a.start_ms < b.start_ms;
    });
    size_t write = 0;
    for (size_t i = 0; i < count; ++i) {
        const ReportSessionRange range = ranges[i];
        if (range.end_ms <= range.start_ms) continue;
        ranges[write++] = range;
    }
    for (size_t i = write; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        ranges[i] = ReportSessionRange{};
    }
    count = write;
}

void normalize_indexed_ranges(ReportIndexedNight &night) {
    normalize_range_array(night.ranges, night.range_count);
}

void normalize_indexed_data_ranges(ReportIndexedNight &night) {
    normalize_range_array(night.data_ranges, night.data_range_count);
}

void normalize_edf_source_signatures(ReportIndexedNight &night) {
    size_t count = std::min(
        night.edf_source_signature_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    std::sort(night.edf_source_signatures,
              night.edf_source_signatures + count);
    size_t write = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t signature = night.edf_source_signatures[i];
        if (signature == 0) continue;
        if (write > 0 &&
            night.edf_source_signatures[write - 1] == signature) {
            continue;
        }
        night.edf_source_signatures[write++] = signature;
    }
    for (size_t i = write; i < AC_REPORT_EDF_SESSION_MAX; ++i) {
        night.edf_source_signatures[i] = 0;
    }
    night.edf_source_signature_count = write;
}

void recompute_source_signature(ReportIndexedNight &night) {
    uint64_t hash = summary_identity_signature(night.summary);
    normalize_edf_source_signatures(night);
    hash = report_hash_u32(
        hash,
        static_cast<uint32_t>(night.edf_source_signature_count));
    for (size_t i = 0; i < night.edf_source_signature_count; ++i) {
        hash = report_hash_u64(hash, night.edf_source_signatures[i]);
    }
    night.source_signature = hash;
}

bool append_edf_source_signature(ReportIndexedNight &night,
                                 uint64_t signature) {
    size_t count = std::min(
        night.edf_source_signature_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    if (count >= AC_REPORT_EDF_SESSION_MAX) return false;
    night.edf_source_signatures[count] = signature;
    night.edf_source_signature_count = count + 1;
    return true;
}

void copy_annotation_file_to_numeric(EdfReportSessionDescriptor &numeric,
                                     const EdfReportSessionDescriptor &annotation,
                                     EdfInventoryFileKind kind) {
    const uint32_t mask = edf_report_file_kind_mask(kind);
    if (mask == 0 || (annotation.file_mask & mask) == 0 ||
        (numeric.file_mask & mask) != 0) {
        return;
    }

    const size_t slot = edf_report_session_file_slot(kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return;
    const EdfReportSessionFileDescriptor &src = annotation.files[slot];
    if (src.kind != kind || !src.path[0]) return;

    numeric.files[slot] = src;
    numeric.file_mask |= mask;
    numeric.file_count++;
    if (src.file_size <= UINT64_MAX - numeric.total_size) {
        numeric.total_size += src.file_size;
    }
    if (src.last_write > numeric.latest_write) {
        numeric.latest_write = src.last_write;
    }
    if (src.header_start_ms > 0 &&
        (numeric.earliest_header_start_ms == 0 ||
         src.header_start_ms < numeric.earliest_header_start_ms)) {
        numeric.earliest_header_start_ms = src.header_start_ms;
    }
    if (src.header_end_ms > numeric.latest_header_end_ms) {
        numeric.latest_header_end_ms = src.header_end_ms;
    }
    numeric.warnings |= annotation.warnings;
}

bool merge_annotation_session_into_numeric(
    EdfReportSessionDescriptor &numeric,
    const EdfReportSessionDescriptor &annotation) {
    if (!edf_session_annotation_matches_numeric(numeric, annotation)) {
        return false;
    }
    copy_annotation_file_to_numeric(numeric,
                                    annotation,
                                    EdfInventoryFileKind::Eve);
    copy_annotation_file_to_numeric(numeric,
                                    annotation,
                                    EdfInventoryFileKind::Csl);
    return true;
}

void sync_summary_sessions_from_ranges(ReportIndexedNight &night) {
    uint32_t duration_sum = 0;
    for (uint32_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        night.summary.sessions[i] = ReportSummarySession{};
    }
    const uint32_t session_count =
        static_cast<uint32_t>(std::min<size_t>(
            night.range_count,
            AC_REPORT_SUMMARY_SESSION_MAX));
    for (uint32_t i = 0; i < session_count; ++i) {
        night.summary.sessions[i].start_ms =
            static_cast<uint64_t>(night.ranges[i].start_ms);
        night.summary.sessions[i].duration_min =
            report_ceil_duration_min(night.ranges[i].start_ms,
                                     night.ranges[i].end_ms);
        duration_sum += night.summary.sessions[i].duration_min;
    }
    night.summary.session_interval_count = session_count;
    night.summary.session_count = session_count;
    night.summary.has_session_count = session_count > 0;
    if (session_count > 0) night.summary.duration_min = duration_sum;
}

void seed_night_ranges_from_summary(ReportIndexedNight &night) {
    night.range_count = collect_session_ranges(night.summary,
                                               night.ranges,
                                               AC_REPORT_SUMMARY_SESSION_MAX);
    normalize_indexed_ranges(night);
}

bool merge_range_into_indexed_night(ReportIndexedNight &night,
                                    int64_t start_ms,
                                    int64_t end_ms) {
    if (end_ms <= start_ms) return false;
    size_t count = std::min(night.range_count,
                            static_cast<size_t>(
                                AC_REPORT_SUMMARY_SESSION_MAX));

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

    if (best >= 0) {
        ReportSessionRange &target =
            night.ranges[static_cast<size_t>(best)];
        target.start_ms = std::min(target.start_ms, start_ms);
        target.end_ms = std::max(target.end_ms, end_ms);
        normalize_indexed_ranges(night);
        sync_summary_sessions_from_ranges(night);
        return true;
    }

    if (count >= AC_REPORT_SUMMARY_SESSION_MAX) return false;
    night.ranges[count].start_ms = start_ms;
    night.ranges[count].end_ms = end_ms;
    night.range_count = count + 1;
    normalize_indexed_ranges(night);
    sync_summary_sessions_from_ranges(night);
    return true;
}

bool append_data_range_to_indexed_night(ReportIndexedNight &night,
                                        int64_t start_ms,
                                        int64_t end_ms) {
    if (end_ms <= start_ms) return false;
    size_t count = std::min(night.data_range_count,
                            static_cast<size_t>(
                                AC_REPORT_SUMMARY_SESSION_MAX));
    if (count >= AC_REPORT_SUMMARY_SESSION_MAX) return false;
    night.data_ranges[count].start_ms = start_ms;
    night.data_ranges[count].end_ms = end_ms;
    night.data_range_count = count + 1;
    normalize_indexed_data_ranges(night);
    return true;
}

bool range_overlaps_any(const ReportSessionRange &range,
                        const ReportSessionRange *ranges,
                        size_t range_count) {
    if (!ranges || range.end_ms <= range.start_ms) return false;
    const size_t count =
        std::min(range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < count; ++i) {
        if (ranges_overlap(range.start_ms,
                           range.end_ms,
                           ranges[i].start_ms,
                           ranges[i].end_ms)) {
            return true;
        }
    }
    return false;
}

bool range_covers_with_tolerance(const ReportSessionRange &outer,
                                 const ReportSessionRange &inner,
                                 int64_t tolerance_ms) {
    if (outer.end_ms <= outer.start_ms || inner.end_ms <= inner.start_ms) {
        return false;
    }
    return outer.start_ms <= inner.start_ms + tolerance_ms &&
           outer.end_ms + tolerance_ms >= inner.end_ms;
}

int find_indexed_night_by_start(ReportIndexedNight *nights,
                                size_t count,
                                uint64_t start_ms) {
    if (!nights || start_ms == 0) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (nights[i].summary.valid &&
            nights[i].summary.start_ms == start_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int find_matching_indexed_range(const ReportIndexedNight &night,
                                int64_t start_ms,
                                int64_t end_ms) {
    if (end_ms <= start_ms) return -1;
    const size_t count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
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
                                       bool have_day_start,
                                       int64_t day_start_ms,
                                       int64_t session_start_ms,
                                       int64_t session_end_ms) {
    if (!nights) return -1;
    if (have_day_start) {
        for (size_t i = 0; i < count; ++i) {
            const ReportSummaryRecord &record = nights[i].summary;
            if (!record.valid) continue;
            const int64_t delta =
                static_cast<int64_t>(record.start_ms) - day_start_ms;
            if (llabs(delta) <= REPORT_SLEEP_DAY_MATCH_TOLERANCE_MS) {
                return static_cast<int>(i);
            }
        }
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

bool ranges_overlap(int64_t start_a,
                    int64_t end_a,
                    int64_t start_b,
                    int64_t end_b) {
    return start_a < end_b && start_b < end_a;
}

size_t collect_session_ranges(const ReportSummaryRecord &night,
                              ReportSessionRange *ranges,
                              size_t max_ranges) {
    if (!ranges || !max_ranges || !night.valid || !night.duration_min) {
        return 0;
    }

    size_t count = 0;
    for (uint32_t i = 0; i < night.session_interval_count &&
                         count < max_ranges; ++i) {
        const ReportSummarySession &session = night.sessions[i];
        if (!session.start_ms || !session.duration_min) continue;
        ReportSessionRange &range = ranges[count];
        range.start_ms = static_cast<int64_t>(session.start_ms);
        range.end_ms = range.start_ms +
                       static_cast<int64_t>(session.duration_min) * 60000LL;
        if (range.end_ms > range.start_ms) count++;
    }

    if (count == 0 && night.end_ms > night.start_ms) {
        ranges[0].start_ms = static_cast<int64_t>(night.start_ms);
        ranges[0].end_ms = static_cast<int64_t>(night.end_ms);
        count = 1;
    }

    std::sort(ranges,
              ranges + count,
              [](const ReportSessionRange &a,
                 const ReportSessionRange &b) {
                  return a.start_ms < b.start_ms;
              });
    return count;
}

bool night_data_span(const ReportSummaryRecord &night,
                     int64_t &span_start,
                     int64_t &span_end) {
    bool found = false;
    for (uint32_t i = 0; i < night.session_interval_count &&
                         i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        const ReportSummarySession &session = night.sessions[i];
        if (!session.start_ms || !session.duration_min) continue;
        const int64_t start = static_cast<int64_t>(session.start_ms);
        const int64_t end =
            start + static_cast<int64_t>(session.duration_min) * 60000LL;
        if (end <= start) continue;
        if (!found) {
            span_start = start;
            span_end = end;
            found = true;
        } else {
            span_start = std::min(span_start, start);
            span_end = std::max(span_end, end);
        }
    }
    if (!found && night.valid && night.duration_min &&
        night.end_ms > night.start_ms) {
        span_start = static_cast<int64_t>(night.start_ms);
        span_end = static_cast<int64_t>(night.end_ms);
        found = true;
    }
    if (!found) return false;
    return span_end > span_start;
}

bool indexed_night_data_span(const ReportIndexedNight &night,
                             int64_t &span_start,
                             int64_t &span_end) {
    bool found = false;
    auto add_range = [&](const ReportSessionRange &range) {
        if (range.end_ms <= range.start_ms) return;
        if (!found) {
            span_start = range.start_ms;
            span_end = range.end_ms;
            found = true;
        } else {
            span_start = std::min(span_start, range.start_ms);
            span_end = std::max(span_end, range.end_ms);
        }
    };

    const size_t edf_count =
        std::min(night.data_range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    if (night.has_edf && edf_count > 0) {
        for (size_t i = 0; i < edf_count; ++i) {
            add_range(night.data_ranges[i]);
        }
    } else {
        const size_t display_count =
            std::min(night.range_count,
                     static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
        for (size_t i = 0; i < display_count; ++i) {
            add_range(night.ranges[i]);
        }
    }
    if (!found) return false;
    return span_end > span_start;
}

bool indexed_night_summary_ranges_covered_by_data(
    const ReportIndexedNight &night) {
    if (!night.has_edf || night.data_range_count == 0 ||
        !night.summary.valid) {
        return false;
    }

    ReportSessionRange summary_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t summary_count =
        collect_session_ranges(night.summary,
                               summary_ranges,
                               AC_REPORT_SUMMARY_SESSION_MAX);
    if (summary_count == 0) return false;

    const size_t data_count =
        std::min(night.data_range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < summary_count; ++i) {
        bool covered = false;
        for (size_t j = 0; j < data_count; ++j) {
            if (range_covers_with_tolerance(night.data_ranges[j],
                                            summary_ranges[i],
                                            REPORT_SESSION_MERGE_TOLERANCE_MS)) {
                covered = true;
                break;
            }
        }
        if (!covered) return false;
    }
    return true;
}

size_t collect_indexed_night_data_ranges(const ReportIndexedNight &night,
                                         ReportSessionRange *ranges,
                                         size_t max_ranges) {
    if (!ranges || max_ranges == 0) return 0;

    size_t count = 0;
    const size_t edf_count =
        std::min(night.data_range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    if (night.has_edf && edf_count > 0) {
        for (size_t i = 0; i < edf_count && count < max_ranges; ++i) {
            if (night.data_ranges[i].end_ms <=
                night.data_ranges[i].start_ms) {
                continue;
            }
            ranges[count++] = night.data_ranges[i];
        }
    } else {
        const size_t display_count =
            std::min(night.range_count,
                     static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
        for (size_t i = 0; i < display_count && count < max_ranges; ++i) {
            if (night.ranges[i].end_ms <= night.ranges[i].start_ms) {
                continue;
            }
            ranges[count++] = night.ranges[i];
        }
    }

    normalize_range_array(ranges, count);
    return count;
}

size_t collect_indexed_night_report_ranges(const ReportIndexedNight &night,
                                           ReportSessionRange *ranges,
                                           size_t max_ranges) {
    if (!ranges || max_ranges == 0) return 0;

    const size_t display_count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    const size_t edf_count =
        std::min(night.data_range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    const bool use_display_ranges =
        display_count > 0 &&
        (!night.has_edf ||
         edf_count == 0 ||
         indexed_night_summary_ranges_covered_by_data(night));

    size_t count = 0;
    if (use_display_ranges) {
        for (size_t i = 0; i < display_count && count < max_ranges; ++i) {
            if (night.ranges[i].end_ms <= night.ranges[i].start_ms) continue;
            ranges[count++] = night.ranges[i];
        }
    } else if (night.has_edf && edf_count > 0) {
        for (size_t i = 0; i < edf_count && count < max_ranges; ++i) {
            if (night.data_ranges[i].end_ms <=
                night.data_ranges[i].start_ms) {
                continue;
            }
            ranges[count++] = night.data_ranges[i];
        }
    }

    normalize_range_array(ranges, count);
    return count;
}

uint32_t report_ceil_duration_min(int64_t start_ms, int64_t end_ms) {
    if (end_ms <= start_ms) return 0;
    const int64_t duration_ms = end_ms - start_ms;
    return static_cast<uint32_t>((duration_ms + 59999LL) / 60000LL);
}

bool report_summary_sleep_day_yyyymmdd(const ReportSummaryRecord &record,
                                       char *out,
                                       size_t out_size) {
    if (!out || out_size < 9 || !record.valid || !record.start_ms ||
        !record.has_tz_offset_min) {
        return false;
    }

    const int64_t local_ms =
        static_cast<int64_t>(record.start_ms) +
        static_cast<int64_t>(record.tz_offset_min) * 60LL * 1000LL;
    if (local_ms < 0) return false;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(local_ms / REPORT_DAY_MS,
                                  year,
                                  month,
                                  day)) {
        return false;
    }
    snprintf(out, out_size, "%04d%02u%02u", year, month, day);
    out[out_size - 1] = '\0';
    return true;
}

uint64_t report_summary_identity_signature(
    const ReportSummaryRecord &record) {
    return summary_identity_signature(record);
}

bool edf_session_has_report_numeric(
    const EdfReportSessionDescriptor &session) {
    const uint32_t numeric_mask =
        edf_report_file_kind_mask(EdfInventoryFileKind::Brp) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Pld) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Sa2);
    return (session.file_mask & numeric_mask) != 0 &&
           session.earliest_header_start_ms > 0 &&
           session.latest_header_end_ms > session.earliest_header_start_ms;
}

bool edf_session_has_report_annotation(
    const EdfReportSessionDescriptor &session) {
    return edf_session_has_report_event_mask(session);
}

bool edf_session_annotation_matches_numeric(
    const EdfReportSessionDescriptor &numeric_session,
    const EdfReportSessionDescriptor &annotation_session) {
    if (!edf_session_has_report_numeric(numeric_session) ||
        !edf_session_has_report_annotation(annotation_session)) {
        return false;
    }
    if (strcmp(numeric_session.sleep_day, annotation_session.sleep_day) != 0) {
        return false;
    }
    if (numeric_session.session_stamp[0] &&
        strcmp(numeric_session.session_stamp,
               annotation_session.session_stamp) == 0) {
        return true;
    }
    return session_windows_match_with_tolerance(
        numeric_session.earliest_header_start_ms,
        numeric_session.latest_header_end_ms,
        annotation_session.earliest_header_start_ms,
        annotation_session.latest_header_end_ms,
        REPORT_SESSION_MERGE_TOLERANCE_MS);
}

void merge_edf_annotation_sessions(EdfReportSessionDescriptor *sessions,
                                   size_t &session_count) {
    if (!sessions || session_count == 0) {
        session_count = 0;
        return;
    }

    const size_t count = std::min(
        session_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    for (size_t i = 0; i < count; ++i) {
        if (!edf_session_has_report_annotation(sessions[i]) ||
            edf_session_has_report_numeric(sessions[i])) {
            continue;
        }

        int best = -1;
        int64_t best_distance = INT64_MAX;
        for (size_t j = 0; j < count; ++j) {
            if (i == j || !edf_session_has_report_numeric(sessions[j])) {
                continue;
            }
            if (!edf_session_annotation_matches_numeric(sessions[j],
                                                        sessions[i])) {
                continue;
            }
            const int64_t distance =
                llabs(sessions[j].earliest_header_start_ms -
                      sessions[i].earliest_header_start_ms);
            if (distance < best_distance) {
                best = static_cast<int>(j);
                best_distance = distance;
            }
        }

        if (best >= 0 &&
            merge_annotation_session_into_numeric(
                sessions[static_cast<size_t>(best)],
                sessions[i])) {
            edf_report_session_init(sessions[i]);
        }
    }

    size_t write = 0;
    for (size_t i = 0; i < session_count; ++i) {
        if (sessions[i].file_count == 0 || sessions[i].file_mask == 0) {
            continue;
        }
        if (write != i) sessions[write] = sessions[i];
        write++;
    }
    for (size_t i = write; i < session_count; ++i) {
        edf_report_session_init(sessions[i]);
    }
    session_count = write;
}

ReportNightIndex::ReportNightIndex(ReportIndexedNight *nights,
                                   size_t capacity)
    : nights_(nights), capacity_(capacity) {
    reset();
}

void ReportNightIndex::reset() {
    count_ = 0;
    if (nights_ && capacity_ > 0) {
        memset(nights_, 0, capacity_ * sizeof(ReportIndexedNight));
    }
}

bool ReportNightIndex::add_summary_record(
    const ReportSummaryRecord &record) {
    if (!record.valid) return true;
    if (!nights_) return false;
    int existing = find_indexed_night_by_start(nights_,
                                               count_,
                                               record.start_ms);
    if (existing < 0 && count_ >= capacity_) return false;

    ReportIndexedNight &night = existing >= 0
        ? nights_[static_cast<size_t>(existing)]
        : nights_[count_];
    const bool had_edf = night.has_edf;
    const size_t old_data_range_count = std::min(
        night.data_range_count,
        static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    ReportSessionRange old_data_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    for (size_t i = 0; i < old_data_range_count; ++i) {
        old_data_ranges[i] = night.data_ranges[i];
    }
    const size_t old_signature_count = std::min(
        night.edf_source_signature_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    uint64_t old_signatures[AC_REPORT_EDF_SESSION_MAX] = {};
    for (size_t i = 0; i < old_signature_count; ++i) {
        old_signatures[i] = night.edf_source_signatures[i];
    }

    night = ReportIndexedNight{};
    night.summary = record;
    night.has_summary = true;
    night.has_edf = had_edf;
    night.data_range_count = old_data_range_count;
    for (size_t i = 0; i < old_data_range_count; ++i) {
        night.data_ranges[i] = old_data_ranges[i];
    }
    night.edf_source_signature_count = old_signature_count;
    for (size_t i = 0; i < old_signature_count; ++i) {
        night.edf_source_signatures[i] = old_signatures[i];
    }
    night.source_signature = summary_identity_signature(record);
    seed_night_ranges_from_summary(night);
    night.edf_catalog_pending =
        had_edf && !indexed_night_summary_ranges_covered_by_data(night);
    if (existing < 0) count_++;
    return true;
}

bool ReportNightIndex::add_indexed_night(const ReportIndexedNight &night) {
    if (!nights_ || !night.summary.valid) return false;
    const int existing = find_indexed_night_by_start(nights_,
                                                     count_,
                                                     night.summary.start_ms);
    if (existing >= 0) {
        nights_[static_cast<size_t>(existing)] = night;
        return true;
    }
    if (count_ >= capacity_) return false;
    nights_[count_++] = night;
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
                                                   have_day_start,
                                                   day_start_ms,
                                                   session_start_ms,
                                                   session_end_ms);
    if (index < 0) {
        if (!has_numeric || !have_day_start || count_ >= capacity_) {
            return true;
        }
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
            append_data_range_to_indexed_night(night,
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
    if (accepted) {
        night.has_edf = true;
        (void)append_edf_source_signature(
            night,
            report_edf_session_signature(session));
        night.edf_catalog_pending =
            night.has_summary &&
            !indexed_night_summary_ranges_covered_by_data(night);
    }
    return true;
}

bool ReportNightIndex::finish(ReportIndexedNight *sort_scratch) {
    if (!nights_) return false;
    for (size_t i = 0; i < count_; ++i) {
        normalize_indexed_ranges(nights_[i]);
        normalize_indexed_data_ranges(nights_[i]);
        recompute_source_signature(nights_[i]);
    }
    if (count_ < 2) return true;
    if (!sort_scratch) return false;
    for (size_t i = 1; i < count_; ++i) {
        *sort_scratch = nights_[i];
        size_t j = i;
        while (j > 0 &&
               nights_[j - 1].summary.start_ms >
                   sort_scratch->summary.start_ms) {
            nights_[j] = nights_[j - 1];
            --j;
        }
        nights_[j] = *sort_scratch;
    }
    return true;
}

bool ReportNightIndex::by_therapy_index(
    const ReportIndexedNight *nights,
    size_t count,
    size_t therapy_index,
    ReportIndexedNight &out) {
    if (!nights) return false;
    size_t current = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t index = i - 1;
        const ReportIndexedNight &night = nights[index];
        const ReportSummaryRecord &record = night.summary;
        if (!record.valid || night.range_count == 0 ||
            record.duration_min == 0) {
            continue;
        }
        if (current == therapy_index) {
            out = night;
            return true;
        }
        current++;
    }
    return false;
}

bool ReportNightIndex::by_start(const ReportIndexedNight *nights,
                                size_t count,
                                uint64_t night_start_ms,
                                ReportIndexedNight &out,
                                size_t *therapy_index_out) {
    if (!nights) return false;
    size_t current = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t index = i - 1;
        const ReportIndexedNight &night = nights[index];
        const ReportSummaryRecord &record = night.summary;
        if (!record.valid || night.range_count == 0 ||
            record.duration_min == 0) {
            continue;
        }
        if (record.start_ms == night_start_ms) {
            out = night;
            if (therapy_index_out) *therapy_index_out = current;
            return true;
        }
        current++;
    }
    return false;
}

}  // namespace aircannect
