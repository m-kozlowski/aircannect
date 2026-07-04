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

}  // namespace aircannect
