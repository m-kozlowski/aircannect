#include "report_range_plot_state.h"

#include <limits.h>
#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_plot_payload.h"

namespace aircannect {

ReportRangePlotBuildState::~ReportRangePlotBuildState() {
    Memory::free(indexed_night);
    Memory::free(chunks);
    Memory::free(edf_sessions);
    Memory::free(ranges);
}

bool ReportRangePlotBuildState::ensure_buffers() {
    if (!indexed_night) {
        indexed_night = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(1, sizeof(ReportIndexedNight), false));
        if (!indexed_night) {
            log_report_alloc_failed("range_indexed_night",
                                    sizeof(ReportIndexedNight));
            return false;
        }
    }

    if (!chunks) {
        chunks = static_cast<ReportResultChunk *>(
            Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                                 sizeof(ReportResultChunk),
                                 false));
        if (!chunks) {
            log_report_alloc_failed(
                "range_chunks",
                AC_REPORT_RESULT_CHUNK_MAX * sizeof(ReportResultChunk));
            return false;
        }
    }

    if (!edf_sessions) {
        edf_sessions = static_cast<EdfReportSessionDescriptor *>(
            Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                                 sizeof(EdfReportSessionDescriptor),
                                 false));
        if (!edf_sessions) {
            log_report_alloc_failed(
                "range_edf_sessions",
                AC_REPORT_EDF_SESSION_MAX *
                    sizeof(EdfReportSessionDescriptor));
            return false;
        }
    }

    if (!ranges) {
        ranges = static_cast<ReportSessionRange *>(
            Memory::calloc_large(AC_REPORT_NIGHT_SESSION_MAX,
                                 sizeof(ReportSessionRange),
                                 false));
        if (!ranges) {
            log_report_alloc_failed(
                "range_plot_ranges",
                AC_REPORT_NIGHT_SESSION_MAX * sizeof(ReportSessionRange));
            return false;
        }
    }

    return true;
}

void ReportRangePlotBuildState::reset() {
    active = false;
    phase = ReportPlotBuildPhase::Idle;
    index = 0;
    from_ms = 0;
    to_ms = 0;
    night_start_ms = 0;

    chunk_count = 0;
    stream_count = 0;
    edf_session_count = 0;
    range_count = 0;
    if (ranges) {
        memset(ranges,
               0,
               AC_REPORT_NIGHT_SESSION_MAX * sizeof(ReportSessionRange));
    }

    bytes.reset();
    tmp.clear();
    seen_events.clear();
    event_count = 0;
    chunk_index = 0;
    stream_index = 0;
    memset(chunk_done, 0, sizeof(chunk_done));

    bucket_ms = 1;
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        series_open[i] = false;
        series_points[i] = 0;
        have_last_sample[i] = false;
        last_sample_ms[i] = 0;
        last_range_index[i] = -1;
        current_bucket[i] = -1;
        buckets[i].clear();
        series_tmp[i].clear();
    }

    ok = true;
    started_ms = 0;
    input_chunks = 0;
    input_bytes = 0;
}

bool ReportRangePlotBuildState::matches(size_t index_,
                                        uint64_t night_start_ms_,
                                        int64_t from_ms_,
                                        int64_t to_ms_) const {
    return index == index_ &&
           night_start_ms == night_start_ms_ &&
           from_ms == from_ms_ &&
           to_ms == to_ms_;
}

bool ReportRangePlotBuildState::open_series(size_t stream_index) {
    if (!bytes || stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        return false;
    }
    if (series_open[stream_index]) return true;

    const ReportResultStream &stream = streams[stream_index];
    const size_t name_len = stream.name ? strlen(stream.name) : 0;
    if (name_len > UINT16_MAX) return false;

    ReportSpoolBuffer &points = series_tmp[stream_index];
    points.clear();
    points.set_max_size(768 * 1024);

    series_points[stream_index] = 0;
    current_bucket[stream_index] = -1;
    have_last_sample[stream_index] = false;
    last_sample_ms[stream_index] = 0;
    last_range_index[stream_index] = -1;
    buckets[stream_index].clear();
    series_open[stream_index] = true;
    return ok;
}

bool ReportRangePlotBuildState::process_series_sample(
    size_t stream_index,
    const ReportSeriesSample &sample,
    ReportSignalId signal,
    ReportSourceId source,
    uint32_t interval_ms,
    int32_t scale,
    bool &capped,
    bool &overflow) {
    if (stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !series_open[stream_index]) {
        return false;
    }
    if (sample.timestamp_ms < from_ms || sample.timestamp_ms >= to_ms) {
        return true;
    }

    const int sample_range_index =
        report_plot_range_index(ranges, range_count, sample.timestamp_ms);
    if (sample_range_index < 0) return true;

    ReportSpoolBuffer &points = series_tmp[stream_index];
    PlotBuildBucket &bucket = buckets[stream_index];

    if (have_last_sample[stream_index] &&
        (sample_range_index != last_range_index[stream_index] ||
         sample.timestamp_ms >
             last_sample_ms[stream_index] +
                 plot_gap_threshold_ms(interval_ms))) {
        series_points[stream_index] +=
            report_emit_plot_gap_to(points, bucket, from_ms, ok);
        current_bucket[stream_index] = -1;

        if (!ok) {
            overflow = true;
            return false;
        }

        if (series_points[stream_index] >= AC_REPORT_RANGE_MAX_POINTS) {
            chunk_index = static_cast<uint32_t>(chunk_count);
            capped = true;
            return false;
        }
    }

    const int64_t sample_bucket_ms =
        plot_bucket_ms_for_signal(signal,
                                  source,
                                  bucket_ms,
                                  interval_ms,
                                  true);
    int64_t sample_bucket = (sample.timestamp_ms - from_ms) / sample_bucket_ms;
    if (sample_bucket < 0) sample_bucket = 0;

    if (current_bucket[stream_index] != sample_bucket) {
        series_points[stream_index] +=
            report_flush_plot_bucket_to(points, bucket, from_ms, ok);

        if (!ok) {
            overflow = true;
            return false;
        }

        current_bucket[stream_index] = sample_bucket;
        if (series_points[stream_index] >= AC_REPORT_RANGE_MAX_POINTS) {
            chunk_index = static_cast<uint32_t>(chunk_count);
            capped = true;
            return false;
        }
    }

    int64_t value = static_cast<int64_t>(sample.value_milli) * scale;
    if (value > INT32_MAX) {
        value = INT32_MAX;
    } else if (value < INT32_MIN) {
        value = INT32_MIN;
    }

    const int32_t value_i32 = static_cast<int32_t>(value);
    if (!bucket.have) {
        bucket.have = true;
        bucket.start_t = sample.timestamp_ms;
        bucket.end_t = sample.timestamp_ms;
        bucket.min_t = sample.timestamp_ms;
        bucket.max_t = sample.timestamp_ms;
        bucket.start_value = value_i32;
        bucket.end_value = value_i32;
        bucket.min_value = value_i32;
        bucket.max_value = value_i32;
    } else {
        bucket.end_t = sample.timestamp_ms;
        bucket.end_value = value_i32;

        if (value_i32 < bucket.min_value) {
            bucket.min_value = value_i32;
            bucket.min_t = sample.timestamp_ms;
        }

        if (value_i32 > bucket.max_value) {
            bucket.max_value = value_i32;
            bucket.max_t = sample.timestamp_ms;
        }
    }

    have_last_sample[stream_index] = true;
    last_sample_ms[stream_index] = sample.timestamp_ms;
    last_range_index[stream_index] = sample_range_index;
    return true;
}

bool ReportRangePlotBuildState::append_decimated_series_gap(
    size_t stream_index) {
    if (stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !series_open[stream_index]) {
        return false;
    }

    ok &= bin_put_i32(series_tmp[stream_index], PLOT_POINT_GAP_DELTA);
    ok &= bin_put_i32(series_tmp[stream_index], 0);
    ++series_points[stream_index];
    return ok;
}

bool ReportRangePlotBuildState::append_decimated_series_point(
    size_t stream_index,
    int64_t timestamp_ms,
    int32_t value_milli) {
    if (stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !series_open[stream_index]) {
        return false;
    }

    ok &= bin_put_i32(series_tmp[stream_index],
                      static_cast<int32_t>(timestamp_ms - from_ms));
    ok &= bin_put_i32(series_tmp[stream_index], value_milli);
    ++series_points[stream_index];
    return ok;
}

bool ReportRangePlotBuildState::finish_series(size_t stream_index,
                                              const char **error_out) {
    if (error_out) *error_out = nullptr;
    if (!bytes || stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        if (error_out) *error_out = "range_bad_state";
        return false;
    }
    if (!series_open[stream_index]) return true;

    ReportSpoolBuffer &points = series_tmp[stream_index];
    PlotBuildBucket &bucket = buckets[stream_index];
    series_points[stream_index] +=
        report_flush_plot_bucket_to(points, bucket, from_ms, ok);
    if (series_points[stream_index] > 0) {
        const ReportResultStream &stream = streams[stream_index];
        if (!append_plot_series_compact(*bytes, stream.name, points, ok)) {
            if (error_out) *error_out = "range_overflow";
            return false;
        }
    }

    points.clear();
    series_open[stream_index] = false;
    series_points[stream_index] = 0;
    current_bucket[stream_index] = -1;
    have_last_sample[stream_index] = false;
    last_sample_ms[stream_index] = 0;
    last_range_index[stream_index] = -1;

    if (!ok) {
        if (error_out) *error_out = "range_overflow";
        return false;
    }

    return true;
}

}  // namespace aircannect
