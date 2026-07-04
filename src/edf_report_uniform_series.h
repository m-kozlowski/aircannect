#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_records.h"

namespace aircannect {

struct EdfReportUniformSeriesBuildContext {
    int64_t start_ms = 0;
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    int32_t *values = nullptr;
    uint8_t *missing_bitmap = nullptr;
    size_t missing_bitmap_bytes = 0;
    bool bad_sample = false;
};

struct EdfReportUniformSeriesData {
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    int32_t *values = nullptr;
    uint8_t *missing_bitmap = nullptr;
    size_t missing_bitmap_bytes = 0;

    void clear();
};

uint32_t edf_report_uniform_series_trim_edge_zero_padding(
    EdfReportUniformSeriesBuildContext &ctx,
    bool trim_leading,
    bool trim_trailing);

bool edf_report_uniform_series_record_sample(
    void *context,
    const ReportSeriesSample &sample);

}  // namespace aircannect
