#include "edf_report_batch_plot.h"

#include <algorithm>
#include <limits.h>
#include <math.h>

namespace aircannect {
namespace {

int32_t physical_to_milli(float value) {
    const long scaled = lroundf(value * 1000.0f);
    if (scaled < INT32_MIN) return INT32_MIN;
    if (scaled > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(scaled);
}

int32_t scaled_digital_to_milli(const EdfSignalScale &scale,
                                int16_t digital,
                                int32_t multiplier) {
    int32_t value = physical_to_milli(edf_scale_digital_sample(scale, digital));
    if (multiplier != 1) {
        const int64_t scaled =
            static_cast<int64_t>(value) * static_cast<int64_t>(multiplier);
        value = scaled > INT32_MAX
                    ? INT32_MAX
                    : (scaled < INT32_MIN ? INT32_MIN
                                          : static_cast<int32_t>(scaled));
    }
    return value;
}

bool emit_plot_point(EdfReportBatchPlotState &plot,
                     const EdfSignalScale &scale,
                     int64_t timestamp_ms,
                     int16_t digital) {
    if (!plot.callback) return false;

    EdfReportSeriesPlotPoint point;
    point.timestamp_ms = timestamp_ms;
    point.value_milli =
        scaled_digital_to_milli(scale, digital, plot.value_multiplier);
    if (!plot.callback(plot.context, plot.item_index, point)) return false;

    plot.points_emitted++;
    return true;
}

bool emit_plot_gap(EdfReportBatchPlotState &plot) {
    if (!plot.callback) return false;

    EdfReportSeriesPlotPoint point;
    point.gap = true;
    if (!plot.callback(plot.context, plot.item_index, point)) return false;

    plot.have_last_sample = false;
    plot.last_sample_ms = 0;
    plot.last_range_index = -1;
    plot.current_bucket = -1;
    plot.current_bucket_start_ms = 0;
    plot.current_bucket_end_ms = 0;
    return true;
}

}  // namespace

void EdfReportBatchPlotBucket::clear() {
    have = false;
    start_t = 0;
    end_t = 0;
    min_t = 0;
    max_t = 0;
    start_digital = 0;
    end_digital = 0;
    min_digital = 0;
    max_digital = 0;
}

int edf_report_batch_plot_find_range(const EdfReportBatchPlotState &plot,
                                     int64_t timestamp_ms) {
    if (!plot.ranges || plot.range_count == 0) return -1;

    if (plot.last_range_index >= 0 &&
        static_cast<size_t>(plot.last_range_index) < plot.range_count) {
        const EdfReportPlotRange &range =
            plot.ranges[plot.last_range_index];
        if (timestamp_ms >= range.start_ms && timestamp_ms < range.end_ms) {
            return plot.last_range_index;
        }
    }

    for (size_t i = 0; i < plot.range_count; ++i) {
        const EdfReportPlotRange &range = plot.ranges[i];
        if (timestamp_ms >= range.start_ms && timestamp_ms < range.end_ms) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool edf_report_batch_plot_flush(EdfReportBatchPlotState &plot,
                                 const EdfSignalScale &scale) {
    EdfReportBatchPlotBucket &bucket = plot.bucket;
    if (!bucket.have) return true;

    struct Point {
        int64_t t = 0;
        int16_t digital = 0;
    };

    Point points[4] = {
        {bucket.start_t, bucket.start_digital},
        {bucket.min_t, bucket.min_digital},
        {bucket.max_t, bucket.max_digital},
        {bucket.end_t, bucket.end_digital},
    };

    for (size_t i = 1; i < 4; ++i) {
        Point v = points[i];
        size_t j = i;
        while (j > 0 && points[j - 1].t > v.t) {
            points[j] = points[j - 1];
            --j;
        }
        points[j] = v;
    }

    bool emitted[4] = {};
    for (uint8_t i = 0; i < 4; ++i) {
        if (emitted[i]) continue;

        if (!emit_plot_point(plot,
                             scale,
                             points[i].t,
                             points[i].digital)) {
            return false;
        }

        for (uint8_t j = i + 1; j < 4; ++j) {
            if (points[j].t == points[i].t) emitted[j] = true;
        }
    }

    bucket.clear();
    return true;
}

bool edf_report_batch_plot_record_sample(EdfReportBatchPlotState &plot,
                                         const EdfSignalScale &scale,
                                         int64_t timestamp_ms,
                                         int16_t digital,
                                         int range_index) {
    if (range_index < 0) return true;

    if (plot.have_last_sample &&
        (range_index != plot.last_range_index ||
         timestamp_ms >
             plot.last_sample_ms +
                 static_cast<int64_t>(plot.gap_threshold_ms))) {
        if (!edf_report_batch_plot_flush(plot, scale)) return false;
        if (!emit_plot_gap(plot)) return false;
    }

    const uint32_t bucket_ms = plot.bucket_ms ? plot.bucket_ms : 1u;
    int64_t sample_bucket = plot.current_bucket;
    if (plot.current_bucket < 0 ||
        timestamp_ms < plot.current_bucket_start_ms ||
        timestamp_ms >= plot.current_bucket_end_ms) {
        sample_bucket =
            (timestamp_ms - plot.plot_start_ms) /
            static_cast<int64_t>(bucket_ms);
        if (sample_bucket < 0) sample_bucket = 0;
    }

    if (plot.current_bucket != sample_bucket) {
        if (!edf_report_batch_plot_flush(plot, scale)) return false;

        plot.current_bucket = sample_bucket;
        plot.current_bucket_start_ms =
            plot.plot_start_ms +
            sample_bucket * static_cast<int64_t>(bucket_ms);
        plot.current_bucket_end_ms =
            plot.current_bucket_start_ms + static_cast<int64_t>(bucket_ms);
    }

    EdfReportBatchPlotBucket &bucket = plot.bucket;
    if (!bucket.have) {
        bucket.have = true;
        bucket.start_t = timestamp_ms;
        bucket.end_t = timestamp_ms;
        bucket.min_t = timestamp_ms;
        bucket.max_t = timestamp_ms;
        bucket.start_digital = digital;
        bucket.end_digital = digital;
        bucket.min_digital = digital;
        bucket.max_digital = digital;
    } else {
        bucket.end_t = timestamp_ms;
        bucket.end_digital = digital;
        if (digital < bucket.min_digital) {
            bucket.min_t = timestamp_ms;
            bucket.min_digital = digital;
        }
        if (digital > bucket.max_digital) {
            bucket.max_t = timestamp_ms;
            bucket.max_digital = digital;
        }
    }

    plot.have_last_sample = true;
    plot.last_sample_ms = timestamp_ms;
    plot.last_range_index = range_index;
    return true;
}

}  // namespace aircannect
