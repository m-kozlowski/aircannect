#pragma once

#include <stdint.h>

#include "report_manager_internal_types.h"
#include "report_plot_format.h"
#include "report_sources.h"
#include "report_spool_types.h"

namespace aircannect {

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

void report_build_empty_plot_bin(ReportSpoolBuffer &out);
int report_plot_range_index(
    const ReportSessionRange *ranges,
    size_t range_count,
    int64_t timestamp_ms);

uint8_t report_flush_plot_bucket_to(
    ReportSpoolBuffer &out,
    report_manager_internal::PlotBuildBucket &bucket,
    int64_t base_ms,
    bool &ok);
uint8_t report_emit_plot_gap_to(
    ReportSpoolBuffer &out,
    report_manager_internal::PlotBuildBucket &bucket,
    int64_t base_ms,
    bool &ok);

void report_flush_plot_envelope_bucket(
    report_manager_internal::PlotSeriesBuildState &state,
    bool &ok);
bool report_append_plot_series_value(
    report_manager_internal::PlotSeriesBuildState &state,
    int64_t base_ms,
    int64_t timestamp_ms,
    int32_t value_milli,
    int64_t bucket_ms,
    bool &ok);
bool report_append_plot_series_point(
    report_manager_internal::PlotSeriesBuildState &state,
    int64_t base_ms,
    int64_t timestamp_ms,
    int32_t value_milli,
    int64_t bucket_ms,
    bool &ok);
bool report_append_plot_series_gap(
    report_manager_internal::PlotSeriesBuildState &state,
    bool &ok);

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
