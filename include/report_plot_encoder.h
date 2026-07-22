#pragma once

#include <stdint.h>

#include "report_plot_format.h"
#include "report_spool_types.h"

namespace aircannect {

bool bin_put_u8(ReportSpoolBuffer &out, uint8_t value);
bool bin_put_u16(ReportSpoolBuffer &out, uint16_t value);
bool bin_put_u32(ReportSpoolBuffer &out, uint32_t value);
bool bin_put_i16(ReportSpoolBuffer &out, int16_t value);
bool bin_put_i32(ReportSpoolBuffer &out, int32_t value);
bool bin_put_i64(ReportSpoolBuffer &out, int64_t value);

uint16_t read_u16_le(const uint8_t *data);
uint32_t read_u32_le(const uint8_t *data);
int32_t read_i32_le(const uint8_t *data);

bool append_plot_series_compact(ReportSpoolBuffer &out,
                                const char *name,
                                const ReportSpoolBuffer &raw_points,
                                bool &ok);
bool append_plot_series_envelope_runs(ReportSpoolBuffer &out,
                                      const char *name,
                                      const ReportSpoolBuffer &raw_buckets,
                                      int64_t bucket_ms,
                                      bool &ok);

}  // namespace aircannect
