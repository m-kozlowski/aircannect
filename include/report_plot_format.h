#pragma once

#include <limits.h>
#include <stdint.h>

namespace aircannect {

// Plot binary wire format (little-endian), served at /api/report/plot:
//   header: magic u32 'ACPB', version u16, flags u16, base_ms i64
//   events: count u32, then count * { t_delta i32, duration i32, code i32, flags i32 }
//   series: to EOF, repeated { name_len u16, name bytes,
//           mode u8, flags u8, reserved u16, mode-specific payload }
// Compact-point payload:
//           series_base_delta_ms i32, time_unit_ms u32, value_scale_milli u32,
//           point_count u32, point_count * { time_index u16, value_i16 }
// Envelope-run payload:
//           axis_base_delta_ms i32, bucket_ms u32, value_scale_milli u32,
//           run_count u32, run_count * {
//             start_bucket u32, bucket_count u16,
//             bucket_count * { min_value_i16, max_value_i16 }
//           }
// Compact time_index 0xFFFF is an explicit segment break. Envelope gaps are
// represented by separate runs. Values are value_i16 * scale / 1000.
constexpr uint32_t PLOT_BIN_MAGIC = 0x42504341u;  // "ACPB"
constexpr uint16_t PLOT_BIN_VERSION = 5;
constexpr uint8_t PLOT_SERIES_MODE_COMPACT = 0;
constexpr uint8_t PLOT_SERIES_MODE_ENVELOPE_RUNS = 1;
constexpr int32_t PLOT_POINT_GAP_DELTA = INT32_MIN;
constexpr uint16_t PLOT_POINT_GAP_INDEX = UINT16_MAX;
constexpr uint32_t PLOT_POINT_MAX_TIME_INDEX =
    static_cast<uint32_t>(PLOT_POINT_GAP_INDEX - 1);
constexpr uint32_t PLOT_ENVELOPE_GAP_BUCKET = UINT32_MAX;
constexpr int64_t PLOT_UNKNOWN_INTERVAL_GAP_MS = 5 * 60 * 1000;

}  // namespace aircannect
