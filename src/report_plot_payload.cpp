#include "report_plot_payload.h"

#include <algorithm>
#include <limits.h>

#include "board_report.h"

namespace aircannect {
namespace {

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

void report_build_empty_plot_bin(ReportSpoolBuffer &out) {
    out.clear();
    out.set_max_size(32);
    bin_put_u32(out, PLOT_BIN_MAGIC);
    bin_put_u16(out, PLOT_BIN_VERSION);
    bin_put_u16(out, 0);   // flags
    bin_put_i64(out, 0);   // base_ms
    bin_put_u32(out, 0);   // event count; no series follow
}

int report_plot_range_index(
    const report_manager_internal::PlotRange *ranges,
    size_t range_count,
    int64_t timestamp_ms) {
    if (!ranges) return -1;

    for (size_t i = 0; i < range_count; ++i) {
        if (timestamp_ms >= ranges[i].start_ms &&
            timestamp_ms < ranges[i].end_ms) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

uint8_t report_flush_plot_bucket_to(
    ReportSpoolBuffer &out,
    report_manager_internal::PlotBuildBucket &bucket,
    int64_t base_ms,
    bool &ok) {
    if (!bucket.have) return 0;

    struct PlotPoint {
        int64_t t = 0;
        int32_t value = 0;
    };

    PlotPoint points[4] = {
        {bucket.start_t, bucket.start_value},
        {bucket.min_t, bucket.min_value},
        {bucket.max_t, bucket.max_value},
        {bucket.end_t, bucket.end_value},
    };

    std::sort(points,
              points + 4,
              [](const PlotPoint &a, const PlotPoint &b) {
                  return a.t < b.t;
              });

    bool emitted[4] = {};
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        if (emitted[i]) continue;

        ok &= bin_put_i32(out, static_cast<int32_t>(points[i].t - base_ms));
        ok &= bin_put_i32(out, points[i].value);
        count++;

        for (uint8_t j = i + 1; j < 4; ++j) {
            if (points[j].t == points[i].t) emitted[j] = true;
        }
    }

    bucket.clear();
    return count;
}

uint8_t report_emit_plot_gap_to(
    ReportSpoolBuffer &out,
    report_manager_internal::PlotBuildBucket &bucket,
    int64_t base_ms,
    bool &ok) {
    uint8_t count = report_flush_plot_bucket_to(out, bucket, base_ms, ok);
    ok &= bin_put_i32(out, PLOT_POINT_GAP_DELTA);
    ok &= bin_put_i32(out, 0);
    return static_cast<uint8_t>(count + 1);
}

void report_flush_plot_envelope_bucket(
    report_manager_internal::PlotSeriesBuildState &state,
    bool &ok) {
    if (!state.bucket.have) return;

    if (state.current_bucket < 0 ||
        state.current_bucket > PLOT_ENVELOPE_GAP_BUCKET - 1) {
        ok = false;
        state.bucket.clear();
        return;
    }

    ok &= bin_put_u32(state.points,
                      static_cast<uint32_t>(state.current_bucket));

    int32_t min_value = state.bucket.min_value;
    int32_t max_value = state.bucket.max_value;
    if (min_value > max_value) std::swap(min_value, max_value);

    ok &= bin_put_i32(state.points, min_value);
    ok &= bin_put_i32(state.points, max_value);
    state.bucket.clear();
}

bool report_append_plot_series_value(
    report_manager_internal::PlotSeriesBuildState &state,
    int64_t base_ms,
    int64_t timestamp_ms,
    int32_t value_milli,
    int64_t bucket_ms,
    bool &ok) {
    if (timestamp_ms < base_ms) return true;
    if (bucket_ms <= 0) bucket_ms = 1;

    if (state.series_bucket_ms <= 0) {
        state.series_bucket_ms = bucket_ms;
    } else {
        bucket_ms = state.series_bucket_ms;
    }

    int64_t sample_bucket = state.current_bucket;
    if (state.current_bucket < 0 ||
        state.current_bucket_ms != bucket_ms ||
        timestamp_ms < state.current_bucket_start_ms ||
        timestamp_ms >= state.current_bucket_end_ms) {
        sample_bucket = (timestamp_ms - base_ms) / bucket_ms;
        if (sample_bucket < 0) sample_bucket = 0;
    }

    if (state.current_bucket != sample_bucket ||
        state.current_bucket_ms != bucket_ms) {
        report_flush_plot_envelope_bucket(state, ok);
        state.current_bucket = sample_bucket;
        state.current_bucket_ms = bucket_ms;
        state.current_bucket_start_ms = base_ms + sample_bucket * bucket_ms;
        state.current_bucket_end_ms =
            state.current_bucket_start_ms + bucket_ms;
    } else if (state.current_bucket_start_ms == 0 ||
               state.current_bucket_end_ms == 0) {
        state.current_bucket_start_ms = base_ms + sample_bucket * bucket_ms;
        state.current_bucket_end_ms =
            state.current_bucket_start_ms + bucket_ms;
    }

    if (!state.bucket.have) {
        state.bucket.have = true;
        state.bucket.start_t = timestamp_ms;
        state.bucket.end_t = timestamp_ms;
        state.bucket.min_t = timestamp_ms;
        state.bucket.max_t = timestamp_ms;
        state.bucket.start_value = value_milli;
        state.bucket.end_value = value_milli;
        state.bucket.min_value = value_milli;
        state.bucket.max_value = value_milli;
    } else {
        state.bucket.end_t = timestamp_ms;
        state.bucket.end_value = value_milli;
        if (value_milli < state.bucket.min_value) {
            state.bucket.min_value = value_milli;
            state.bucket.min_t = timestamp_ms;
        }
        if (value_milli > state.bucket.max_value) {
            state.bucket.max_value = value_milli;
            state.bucket.max_t = timestamp_ms;
        }
    }

    return ok;
}

bool report_append_plot_series_point(
    report_manager_internal::PlotSeriesBuildState &state,
    int64_t base_ms,
    int64_t timestamp_ms,
    int32_t value_milli,
    int64_t bucket_ms,
    bool &ok) {
    if (!report_append_plot_series_value(state,
                                         base_ms,
                                         timestamp_ms,
                                         value_milli,
                                         bucket_ms,
                                         ok)) {
        return false;
    }

    state.have_last_sample = true;
    state.last_sample_ms = timestamp_ms;
    return ok;
}

bool report_append_plot_series_gap(
    report_manager_internal::PlotSeriesBuildState &state,
    bool &ok) {
    report_flush_plot_envelope_bucket(state, ok);

    ok &= bin_put_u32(state.points, PLOT_ENVELOPE_GAP_BUCKET);
    ok &= bin_put_i32(state.points, 0);
    ok &= bin_put_i32(state.points, 0);

    state.have_last_sample = false;
    state.last_sample_ms = 0;
    state.last_range_index = -1;
    state.current_bucket = -1;
    state.current_bucket_start_ms = 0;
    state.current_bucket_end_ms = 0;
    state.current_bucket_ms = 0;
    state.bucket.clear();
    return ok;
}

}  // namespace aircannect
