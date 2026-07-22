#include "report_plot_encoder.h"

#include <algorithm>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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

}  // namespace

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
