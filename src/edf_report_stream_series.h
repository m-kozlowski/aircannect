#pragma once

#include <stdint.h>

#include "edf_report_series_reader.h"

namespace aircannect {

struct EdfReportStreamSeriesContext {
    ReportSeriesSampleCallback callback = nullptr;
    void *context = nullptr;
    uint32_t interval_ms = 0;
    bool trim_leading = false;
    bool trim_trailing = false;
    bool leading_open = false;
    bool pending_zero = false;
    int64_t pending_zero_start_ms = 0;
    int64_t pending_zero_next_ms = 0;
    uint32_t pending_zero_count = 0;
    uint32_t samples_emitted = 0;
    uint32_t samples_trimmed = 0;
};

bool edf_report_stream_series_record_sample(
    void *context,
    const ReportSeriesSample &sample);

void edf_report_stream_series_clear_zero_run(
    EdfReportStreamSeriesContext &ctx);

}  // namespace aircannect
