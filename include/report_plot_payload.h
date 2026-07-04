#pragma once

#include <limits.h>
#include <stdint.h>

#include "report_sources.h"
#include "report_spool_types.h"

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

bool bin_put_u16(ReportSpoolBuffer &b, uint16_t v);
bool bin_put_u8(ReportSpoolBuffer &b, uint8_t v);
bool bin_put_u32(ReportSpoolBuffer &b, uint32_t v);
bool bin_put_i16(ReportSpoolBuffer &b, int16_t v);
bool bin_put_i32(ReportSpoolBuffer &b, int32_t v);
bool bin_put_i64(ReportSpoolBuffer &b, int64_t v);

uint16_t read_u16_le(const uint8_t *p);
uint32_t read_u32_le(const uint8_t *p);
int32_t read_i32_le(const uint8_t *p);

int64_t plot_gap_threshold_ms(uint32_t interval_ms);
uint32_t infer_chunk_interval_ms(uint32_t record_count,
                                 int64_t start_ms,
                                 int64_t end_ms);
int64_t plot_bucket_ms_for_signal(ReportSignalId signal,
                                  ReportSourceId source,
                                  int64_t target_bucket_ms,
                                  uint32_t interval_ms,
                                  bool preserve_slow_native_cadence);
int32_t plot_value_multiplier(ReportSignalId signal, ReportSourceId source);

bool append_plot_series_compact(ReportSpoolBuffer &out,
                                const char *name,
                                const ReportSpoolBuffer &raw_points,
                                bool &ok);
bool append_plot_series_envelope_runs(ReportSpoolBuffer &out,
                                      const char *name,
                                      const ReportSpoolBuffer &raw_buckets,
                                      int64_t bucket_ms,
                                      bool &ok);

struct PlotBlobScan {
    bool valid = false;
    uint32_t events = 0;
    uint32_t points = 0;
};

PlotBlobScan scan_plot_blob(const ReportSpoolBuffer &b);

}  // namespace aircannect
