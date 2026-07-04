#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_reader.h"
#include "edf_report_data_reader.h"

namespace aircannect {

struct EdfReportBatchPlotBucket {
    bool have = false;
    int64_t start_t = 0;
    int64_t end_t = 0;
    int64_t min_t = 0;
    int64_t max_t = 0;
    int16_t start_digital = 0;
    int16_t end_digital = 0;
    int16_t min_digital = 0;
    int16_t max_digital = 0;

    void clear();
};

struct EdfReportBatchPlotState {
    const EdfReportPlotRange *ranges = nullptr;
    size_t range_count = 0;
    int64_t plot_start_ms = 0;
    uint32_t bucket_ms = 1;
    uint32_t gap_threshold_ms = 5000;
    int32_t value_multiplier = 1;
    EdfReportBatchPlotBucket bucket;
    bool have_last_sample = false;
    int64_t last_sample_ms = 0;
    int last_range_index = -1;
    int64_t current_bucket = -1;
    int64_t current_bucket_start_ms = 0;
    int64_t current_bucket_end_ms = 0;
    EdfReportSeriesBatchPlotCallback callback = nullptr;
    void *context = nullptr;
    size_t item_index = 0;
    uint32_t points_emitted = 0;
};

int edf_report_batch_plot_find_range(const EdfReportBatchPlotState &plot,
                                     int64_t timestamp_ms);

bool edf_report_batch_plot_record_sample(EdfReportBatchPlotState &plot,
                                         const EdfSignalScale &scale,
                                         int64_t timestamp_ms,
                                         int16_t digital,
                                         int range_index);

bool edf_report_batch_plot_flush(EdfReportBatchPlotState &plot,
                                 const EdfSignalScale &scale);

}  // namespace aircannect
