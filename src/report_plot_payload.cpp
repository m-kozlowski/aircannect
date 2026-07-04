#include "report_plot_payload.h"

#include <algorithm>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "board_report.h"

namespace aircannect {
namespace {

uint32_t ceil_div_u64(uint64_t n, uint64_t d) {
    return d ? static_cast<uint32_t>((n + d - 1) / d) : 0;
}

int16_t quantize_plot_value(int32_t value_milli,
                            uint32_t scale_milli) {
    if (scale_milli == 0) scale_milli = 1;

    int64_t value = value_milli;
    int64_t encoded = 0;
    if (value >= 0) {
        encoded = (value + static_cast<int64_t>(scale_milli / 2)) /
                  static_cast<int64_t>(scale_milli);
    } else {
        encoded = -((-value + static_cast<int64_t>(scale_milli / 2)) /
                    static_cast<int64_t>(scale_milli));
    }

    if (encoded > INT16_MAX) encoded = INT16_MAX;
    if (encoded < INT16_MIN) encoded = INT16_MIN;
    return static_cast<int16_t>(encoded);
}

int64_t plot_bucket_ms_for_interval(int64_t target_bucket_ms,
                                    uint32_t interval_ms,
                                    bool preserve_slow_native_cadence) {
    if (target_bucket_ms < 1) target_bucket_ms = 1;
    if (preserve_slow_native_cadence &&
        interval_ms >= AC_REPORT_RANGE_PLOT_PRESERVE_INTERVAL_MIN_MS) {
        return std::min<int64_t>(target_bucket_ms,
                                 static_cast<int64_t>(interval_ms));
    }
    return target_bucket_ms;
}

}  // namespace

bool bin_put_u16(ReportSpoolBuffer &b, uint16_t v) {
    const uint8_t x[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    return b.append(x, sizeof(x));
}

bool bin_put_u8(ReportSpoolBuffer &b, uint8_t v) {
    return b.append(&v, sizeof(v));
}

bool bin_put_u32(ReportSpoolBuffer &b, uint32_t v) {
    const uint8_t x[4] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24),
    };
    return b.append(x, sizeof(x));
}

bool bin_put_i16(ReportSpoolBuffer &b, int16_t v) {
    return bin_put_u16(b, static_cast<uint16_t>(v));
}

bool bin_put_i32(ReportSpoolBuffer &b, int32_t v) {
    return bin_put_u32(b, static_cast<uint32_t>(v));
}

bool bin_put_i64(ReportSpoolBuffer &b, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    return bin_put_u32(b, static_cast<uint32_t>(u)) &&
           bin_put_u32(b, static_cast<uint32_t>(u >> 32));
}

uint16_t read_u16_le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t read_i32_le(const uint8_t *p) {
    return static_cast<int32_t>(read_u32_le(p));
}

int64_t plot_gap_threshold_ms(uint32_t interval_ms) {
    if (interval_ms == 0) return PLOT_UNKNOWN_INTERVAL_GAP_MS;

    const int64_t by_interval = static_cast<int64_t>(interval_ms) * 3LL;
    return std::max<int64_t>(5000, by_interval);
}

uint32_t infer_chunk_interval_ms(uint32_t record_count,
                                 int64_t start_ms,
                                 int64_t end_ms) {
    if (record_count == 0 || end_ms <= start_ms) return 0;

    const int64_t duration_ms = end_ms - start_ms;
    const int64_t interval_ms =
        duration_ms / static_cast<int64_t>(record_count);
    return interval_ms > 0 && interval_ms <= UINT32_MAX
               ? static_cast<uint32_t>(interval_ms)
               : 0;
}

int64_t plot_bucket_ms_for_signal(ReportSignalId signal,
                                  ReportSourceId source,
                                  int64_t target_bucket_ms,
                                  uint32_t interval_ms,
                                  bool preserve_slow_native_cadence) {
    int64_t bucket_ms = plot_bucket_ms_for_interval(
        target_bucket_ms, interval_ms, preserve_slow_native_cadence);
    if (!preserve_slow_native_cadence) {
        if (interval_ms > 0) {
            bucket_ms =
                std::max<int64_t>(bucket_ms, static_cast<int64_t>(interval_ms));
        }
        return std::max<int64_t>(1, bucket_ms);
    }

    if (interval_ms > 0) {
        int64_t cap_ms = 0;
        if ((signal == ReportSignalId::Flow &&
             source == ReportSourceId::RespiratoryFlow6p25Hz) ||
            (signal == ReportSignalId::MaskPressure &&
             source == ReportSourceId::MaskPressure6p25Hz) ||
            interval_ms <
                AC_REPORT_LOW_RATE_NATIVE_PLOT_MIN_INTERVAL_MS) {
            cap_ms = AC_REPORT_HIGH_RATE_PLOT_BUCKET_MAX_MS;
        } else if (interval_ms <=
                   AC_REPORT_LOW_RATE_NATIVE_PLOT_MAX_INTERVAL_MS) {
            cap_ms = preserve_slow_native_cadence
                         ? static_cast<int64_t>(interval_ms)
                         : AC_REPORT_LOW_RATE_OVERVIEW_BUCKET_MAX_MS;
        }
        if (cap_ms > 0) bucket_ms = std::min<int64_t>(bucket_ms, cap_ms);
        bucket_ms =
            std::max<int64_t>(bucket_ms, static_cast<int64_t>(interval_ms));
    }

    return std::max<int64_t>(1, bucket_ms);
}

int32_t plot_value_multiplier(ReportSignalId signal, ReportSourceId source) {
    if ((signal == ReportSignalId::Flow &&
         source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (signal == ReportSignalId::Leak &&
         source == ReportSourceId::Leak0p5Hz)) {
        return 60;
    }
    return 1;
}

bool append_plot_series_compact(ReportSpoolBuffer &out,
                                const char *name,
                                const ReportSpoolBuffer &raw_points,
                                bool &ok) {
    if (!ok) return false;
    if (raw_points.size() == 0) return true;
    if ((raw_points.size() % 8) != 0) {
        ok = false;
        return false;
    }

    const size_t point_count = raw_points.size() / 8;
    if (point_count > UINT32_MAX) {
        ok = false;
        return false;
    }

    const size_t name_len = name ? strlen(name) : 0;
    if (name_len > UINT16_MAX) {
        ok = false;
        return false;
    }

    const uint8_t *raw = raw_points.data();
    bool have_real_point = false;
    int32_t min_delta = INT32_MAX;
    int32_t max_delta = INT32_MIN;
    int64_t max_abs_value = 0;
    for (size_t i = 0; i < point_count; ++i) {
        const uint8_t *p = raw + i * 8;
        const int32_t delta = read_i32_le(p);
        if (delta == PLOT_POINT_GAP_DELTA) continue;

        const int32_t value = read_i32_le(p + 4);
        min_delta = std::min(min_delta, delta);
        max_delta = std::max(max_delta, delta);

        const int64_t abs_value =
            value == INT32_MIN
                ? static_cast<int64_t>(INT32_MAX) + 1
                : llabs(static_cast<int64_t>(value));
        max_abs_value = std::max(max_abs_value, abs_value);
        have_real_point = true;
    }
    if (!have_real_point) return true;

    const uint64_t span = max_delta > min_delta
                              ? static_cast<uint64_t>(
                                    static_cast<int64_t>(max_delta) -
                                    static_cast<int64_t>(min_delta))
                              : 0;
    uint32_t time_unit_ms = std::max<uint32_t>(
        1, ceil_div_u64(span, PLOT_POINT_MAX_TIME_INDEX));
    if (time_unit_ms == 0) time_unit_ms = 1;

    const uint32_t value_scale_milli =
        std::max<uint32_t>(1, ceil_div_u64(static_cast<uint64_t>(max_abs_value),
                                          static_cast<uint64_t>(INT16_MAX)));

    ok &= bin_put_u16(out, static_cast<uint16_t>(name_len));
    if (name_len) {
        ok &= out.append(reinterpret_cast<const uint8_t *>(name), name_len);
    }
    ok &= bin_put_u8(out, PLOT_SERIES_MODE_COMPACT);
    ok &= bin_put_u8(out, 0);
    ok &= bin_put_u16(out, 0);
    ok &= bin_put_i32(out, min_delta);
    ok &= bin_put_u32(out, time_unit_ms);
    ok &= bin_put_u32(out, value_scale_milli);
    ok &= bin_put_u32(out, static_cast<uint32_t>(point_count));

    for (size_t i = 0; i < point_count; ++i) {
        const uint8_t *p = raw + i * 8;
        const int32_t delta = read_i32_le(p);
        const int32_t value = read_i32_le(p + 4);
        if (delta == PLOT_POINT_GAP_DELTA) {
            ok &= bin_put_u16(out, PLOT_POINT_GAP_INDEX);
            ok &= bin_put_i16(out, 0);
            continue;
        }

        const uint64_t offset =
            static_cast<uint64_t>(static_cast<int64_t>(delta) -
                                  static_cast<int64_t>(min_delta));
        uint64_t index =
            (offset + static_cast<uint64_t>(time_unit_ms / 2)) /
            static_cast<uint64_t>(time_unit_ms);
        if (index > PLOT_POINT_MAX_TIME_INDEX) {
            index = PLOT_POINT_MAX_TIME_INDEX;
        }

        ok &= bin_put_u16(out, static_cast<uint16_t>(index));
        ok &= bin_put_i16(out, quantize_plot_value(value, value_scale_milli));
    }

    return ok;
}

bool append_plot_series_envelope_runs(ReportSpoolBuffer &out,
                                      const char *name,
                                      const ReportSpoolBuffer &raw_buckets,
                                      int64_t bucket_ms,
                                      bool &ok) {
    if (!ok) return false;
    if (raw_buckets.size() == 0) return true;
    if (bucket_ms <= 0 || bucket_ms > UINT32_MAX ||
        (raw_buckets.size() % 12) != 0) {
        ok = false;
        return false;
    }

    const size_t record_count = raw_buckets.size() / 12;
    const size_t name_len = name ? strlen(name) : 0;
    if (name_len > UINT16_MAX) {
        ok = false;
        return false;
    }

    const uint8_t *raw = raw_buckets.data();
    bool have_real_bucket = false;
    int64_t max_abs_value = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const uint8_t *p = raw + i * 12;
        const uint32_t bucket = read_u32_le(p);
        if (bucket == PLOT_ENVELOPE_GAP_BUCKET) continue;

        const int32_t min_value = read_i32_le(p + 4);
        const int32_t max_value = read_i32_le(p + 8);
        const int64_t min_abs =
            min_value == INT32_MIN
                ? static_cast<int64_t>(INT32_MAX) + 1
                : llabs(static_cast<int64_t>(min_value));
        const int64_t max_abs =
            max_value == INT32_MIN
                ? static_cast<int64_t>(INT32_MAX) + 1
                : llabs(static_cast<int64_t>(max_value));
        max_abs_value = std::max(max_abs_value, std::max(min_abs, max_abs));
        have_real_bucket = true;
    }
    if (!have_real_bucket) return true;

    uint32_t run_count = 0;
    bool in_run = false;
    uint32_t previous_bucket = 0;
    uint32_t buckets_in_run = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const uint32_t bucket = read_u32_le(raw + i * 12);
        if (bucket == PLOT_ENVELOPE_GAP_BUCKET) {
            in_run = false;
            buckets_in_run = 0;
            continue;
        }

        const bool starts_run =
            !in_run || bucket != previous_bucket + 1 ||
            buckets_in_run >= UINT16_MAX;
        if (starts_run) {
            ++run_count;
            buckets_in_run = 0;
            in_run = true;
        }
        previous_bucket = bucket;
        ++buckets_in_run;
    }
    if (run_count == 0) return true;

    const uint32_t value_scale_milli =
        std::max<uint32_t>(1, ceil_div_u64(static_cast<uint64_t>(max_abs_value),
                                          static_cast<uint64_t>(INT16_MAX)));

    ok &= bin_put_u16(out, static_cast<uint16_t>(name_len));
    if (name_len) {
        ok &= out.append(reinterpret_cast<const uint8_t *>(name), name_len);
    }
    ok &= bin_put_u8(out, PLOT_SERIES_MODE_ENVELOPE_RUNS);
    ok &= bin_put_u8(out, 0);
    ok &= bin_put_u16(out, 0);
    ok &= bin_put_i32(out, 0);
    ok &= bin_put_u32(out, static_cast<uint32_t>(bucket_ms));
    ok &= bin_put_u32(out, value_scale_milli);
    ok &= bin_put_u32(out, run_count);

    size_t i = 0;
    while (i < record_count) {
        uint32_t bucket = read_u32_le(raw + i * 12);
        if (bucket == PLOT_ENVELOPE_GAP_BUCKET) {
            ++i;
            continue;
        }

        const size_t run_start = i;
        const uint32_t start_bucket = bucket;
        uint16_t bucket_count = 1;
        ++i;
        while (i < record_count && bucket_count < UINT16_MAX) {
            const uint32_t next_bucket = read_u32_le(raw + i * 12);
            if (next_bucket == PLOT_ENVELOPE_GAP_BUCKET ||
                next_bucket != bucket + 1) {
                break;
            }
            bucket = next_bucket;
            ++bucket_count;
            ++i;
        }

        ok &= bin_put_u32(out, start_bucket);
        ok &= bin_put_u16(out, bucket_count);
        for (uint16_t n = 0; n < bucket_count; ++n) {
            const uint8_t *p = raw + (run_start + n) * 12;
            int32_t min_value = read_i32_le(p + 4);
            int32_t max_value = read_i32_le(p + 8);
            if (min_value > max_value) std::swap(min_value, max_value);
            ok &= bin_put_i16(out,
                              quantize_plot_value(min_value,
                                                  value_scale_milli));
            ok &= bin_put_i16(out,
                              quantize_plot_value(max_value,
                                                  value_scale_milli));
        }
    }

    return ok;
}

}  // namespace aircannect
