#include "report_night_index_internal.h"

#include <algorithm>
#include <stdio.h>

#include "calendar_utils.h"

namespace aircannect {
namespace {

bool range_covers_with_tolerance(const ReportSessionRange &outer,
                                 const ReportSessionRange &inner,
                                 int64_t tolerance_ms) {
    if (outer.end_ms <= outer.start_ms || inner.end_ms <= inner.start_ms) {
        return false;
    }

    return outer.start_ms <= inner.start_ms + tolerance_ms &&
           outer.end_ms + tolerance_ms >= inner.end_ms;
}

}  // namespace

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

void coalesce_sorted_range_array(ReportSessionRange *ranges, size_t &count) {
    if (!ranges || count < 2) return;

    size_t write = 0;
    for (size_t i = 0; i < count; ++i) {
        const ReportSessionRange range = ranges[i];
        if (range.end_ms <= range.start_ms) continue;

        if (write > 0) {
            ReportSessionRange &previous = ranges[write - 1];
            const int64_t gap_ms =
                range.start_ms > previous.end_ms
                    ? range.start_ms - previous.end_ms
                    : 0;

            if (gap_ms <= REPORT_SESSION_MERGE_TOLERANCE_MS) {
                previous.start_ms = std::min(previous.start_ms,
                                             range.start_ms);
                previous.end_ms = std::max(previous.end_ms, range.end_ms);
                continue;
            }
        }

        ranges[write++] = range;
    }

    for (size_t i = write; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        ranges[i] = ReportSessionRange{};
    }
    count = write;
}

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

    ReportSessionRange data_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t data_count =
        collect_indexed_night_data_ranges(night,
                                          data_ranges,
                                          AC_REPORT_SUMMARY_SESSION_MAX);
    if (night.has_edf && data_count > 0) {
        for (size_t i = 0; i < data_count; ++i) add_range(data_ranges[i]);
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

    ReportSessionRange data_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t data_count =
        collect_indexed_night_data_ranges(night,
                                          data_ranges,
                                          AC_REPORT_SUMMARY_SESSION_MAX);
    for (size_t i = 0; i < summary_count; ++i) {
        bool covered = false;
        for (size_t j = 0; j < data_count; ++j) {
            if (range_covers_with_tolerance(data_ranges[j],
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
    coalesce_sorted_range_array(ranges, count);
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
    if (!use_display_ranges) {
        coalesce_sorted_range_array(ranges, count);
    }
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

}  // namespace aircannect
