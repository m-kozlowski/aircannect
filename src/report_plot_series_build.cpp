#include "report_manager.h"

#include <algorithm>
#include <limits.h>
#include <stdint.h>

#include "report_data_provider.h"
#include "report_plot_payload.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

bool ReportManager::open_plot_series_state(size_t stream_index) {
    if (stream_index >= result_stream_count_ ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        return false;
    }

    PlotSeriesBuildState &state = plot_series_states_[stream_index];
    if (state.open) return true;

    state.reset();
    state.points.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    state.open = true;
    return plot_bin_ok_;
}

uint8_t ReportManager::flush_plot_bucket_to(ReportSpoolBuffer &out,
                                            PlotBuildBucket &bucket,
                                            int64_t base_ms,
                                            bool &ok) {
    if (!bucket.have) return 0;

    struct PlotPoint {
        int64_t t = 0;
        int32_t value = 0;
    };

    PlotPoint points[4] = {
        {bucket.start_t, bucket.start_value},
        {bucket.min_t, bucket.min_value},
        {bucket.max_t, bucket.max_value},
        {bucket.end_t, bucket.end_value},
    };

    std::sort(points,
              points + 4,
              [](const PlotPoint &a, const PlotPoint &b) {
                  return a.t < b.t;
              });

    bool emitted[4] = {};
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        if (emitted[i]) continue;

        ok &= bin_put_i32(out, static_cast<int32_t>(points[i].t - base_ms));
        ok &= bin_put_i32(out, points[i].value);
        count++;

        for (uint8_t j = i + 1; j < 4; ++j) {
            if (points[j].t == points[i].t) emitted[j] = true;
        }
    }

    bucket.clear();
    return count;
}

uint8_t ReportManager::emit_plot_gap_to(ReportSpoolBuffer &out,
                                        PlotBuildBucket &bucket,
                                        int64_t base_ms,
                                        bool &ok) {
    uint8_t count = flush_plot_bucket_to(out, bucket, base_ms, ok);
    ok &= bin_put_i32(out, PLOT_POINT_GAP_DELTA);
    ok &= bin_put_i32(out, 0);
    return static_cast<uint8_t>(count + 1);
}

void ReportManager::flush_plot_bucket(PlotSeriesBuildState &state) {
    if (!state.bucket.have) return;

    if (state.current_bucket < 0 ||
        state.current_bucket > PLOT_ENVELOPE_GAP_BUCKET - 1) {
        plot_bin_ok_ = false;
        state.bucket.clear();
        return;
    }

    plot_bin_ok_ &=
        bin_put_u32(state.points, static_cast<uint32_t>(state.current_bucket));

    int32_t min_value = state.bucket.min_value;
    int32_t max_value = state.bucket.max_value;
    if (min_value > max_value) std::swap(min_value, max_value);

    plot_bin_ok_ &= bin_put_i32(state.points, min_value);
    plot_bin_ok_ &= bin_put_i32(state.points, max_value);
    state.bucket.clear();
}

bool ReportManager::append_plot_series_value(PlotSeriesBuildState &state,
                                             int64_t timestamp_ms,
                                             int32_t value_milli,
                                             int64_t bucket_ms) {
    if (timestamp_ms < plot_start_ms_) return true;
    if (bucket_ms <= 0) bucket_ms = 1;

    if (state.series_bucket_ms <= 0) {
        state.series_bucket_ms = bucket_ms;
    } else {
        bucket_ms = state.series_bucket_ms;
    }

    int64_t sample_bucket = state.current_bucket;
    if (state.current_bucket < 0 ||
        state.current_bucket_ms != bucket_ms ||
        timestamp_ms < state.current_bucket_start_ms ||
        timestamp_ms >= state.current_bucket_end_ms) {
        sample_bucket = (timestamp_ms - plot_start_ms_) / bucket_ms;
        if (sample_bucket < 0) sample_bucket = 0;
    }

    if (state.current_bucket != sample_bucket ||
        state.current_bucket_ms != bucket_ms) {
        flush_plot_bucket(state);
        state.current_bucket = sample_bucket;
        state.current_bucket_ms = bucket_ms;
        state.current_bucket_start_ms =
            plot_start_ms_ + sample_bucket * bucket_ms;
        state.current_bucket_end_ms =
            state.current_bucket_start_ms + bucket_ms;
    } else if (state.current_bucket_start_ms == 0 ||
               state.current_bucket_end_ms == 0) {
        state.current_bucket_start_ms =
            plot_start_ms_ + sample_bucket * bucket_ms;
        state.current_bucket_end_ms =
            state.current_bucket_start_ms + bucket_ms;
    }

    if (!state.bucket.have) {
        state.bucket.have = true;
        state.bucket.start_t = timestamp_ms;
        state.bucket.end_t = timestamp_ms;
        state.bucket.min_t = timestamp_ms;
        state.bucket.max_t = timestamp_ms;
        state.bucket.start_value = value_milli;
        state.bucket.end_value = value_milli;
        state.bucket.min_value = value_milli;
        state.bucket.max_value = value_milli;
    } else {
        state.bucket.end_t = timestamp_ms;
        state.bucket.end_value = value_milli;
        if (value_milli < state.bucket.min_value) {
            state.bucket.min_value = value_milli;
            state.bucket.min_t = timestamp_ms;
        }
        if (value_milli > state.bucket.max_value) {
            state.bucket.max_value = value_milli;
            state.bucket.max_t = timestamp_ms;
        }
    }

    return plot_bin_ok_;
}

bool ReportManager::process_plot_series_sample_value(
    PlotSeriesBuildState &state,
    const ReportResultChunk &chunk,
    const ReportSeriesSample &sample,
    uint32_t interval_ms) {
    int range_index = -1;
    if (state.last_range_index >= 0 &&
        static_cast<size_t>(state.last_range_index) < plot_range_count_) {
        const PlotRange &last_range = plot_ranges_[state.last_range_index];
        if (sample.timestamp_ms >= last_range.start_ms &&
            sample.timestamp_ms < last_range.end_ms) {
            range_index = state.last_range_index;
        }
    }
    if (range_index < 0) {
        range_index = plot_range_index(sample.timestamp_ms);
    }
    if (range_index < 0) {
        return true;
    }

    if (state.have_last_sample &&
        (range_index != state.last_range_index ||
         sample.timestamp_ms >
             state.last_sample_ms + plot_gap_threshold_ms(interval_ms))) {
        if (!append_plot_series_gap(state)) return false;
    }

    const int64_t bucket_ms =
        plot_bucket_ms_for_signal(chunk.signal,
                                  chunk.source,
                                  plot_bucket_ms_,
                                  interval_ms,
                                  false);

    int32_t value_milli = sample.value_milli;
    if ((chunk.signal == ReportSignalId::Flow &&
         chunk.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (chunk.signal == ReportSignalId::Leak &&
         chunk.source == ReportSourceId::Leak0p5Hz)) {
        const int64_t scaled = static_cast<int64_t>(value_milli) * 60LL;
        value_milli = scaled > INT32_MAX
                          ? INT32_MAX
                          : (scaled < INT32_MIN
                                 ? INT32_MIN
                                 : static_cast<int32_t>(scaled));
    }

    if (!append_plot_series_value(state,
                                  sample.timestamp_ms,
                                  value_milli,
                                  bucket_ms)) {
        return false;
    }

    state.bucket.range_index = range_index;
    state.have_last_sample = true;
    state.last_sample_ms = sample.timestamp_ms;
    state.last_range_index = range_index;
    return true;
}

bool ReportManager::append_plot_series_point(PlotSeriesBuildState &state,
                                             int64_t timestamp_ms,
                                             int32_t value_milli,
                                             int64_t bucket_ms) {
    if (!append_plot_series_value(state,
                                  timestamp_ms,
                                  value_milli,
                                  bucket_ms)) {
        return false;
    }

    state.have_last_sample = true;
    state.last_sample_ms = timestamp_ms;
    return plot_bin_ok_;
}

bool ReportManager::append_plot_series_gap(PlotSeriesBuildState &state) {
    flush_plot_bucket(state);

    plot_bin_ok_ &= bin_put_u32(state.points, PLOT_ENVELOPE_GAP_BUCKET);
    plot_bin_ok_ &= bin_put_i32(state.points, 0);
    plot_bin_ok_ &= bin_put_i32(state.points, 0);

    state.have_last_sample = false;
    state.last_sample_ms = 0;
    state.last_range_index = -1;
    state.current_bucket = -1;
    state.current_bucket_start_ms = 0;
    state.current_bucket_end_ms = 0;
    state.current_bucket_ms = 0;
    state.bucket.clear();
    return plot_bin_ok_;
}

bool ReportManager::process_plot_series_chunk(size_t chunk_index) {
    if (chunk_index >= result_status_.chunk_count ||
        chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail_result_prepare("plot_bad_chunk");
        return false;
    }

    const ReportResultChunk &chunk = result_chunks_[chunk_index];
    if (chunk.stream_index >= result_stream_count_ ||
        chunk.stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !open_plot_series_state(chunk.stream_index)) {
        fail_result_prepare("plot_series_open_failed");
        return false;
    }

    struct PlotSeriesContext {
        ReportManager *manager = nullptr;
        const ReportResultChunk *chunk = nullptr;
        PlotSeriesBuildState *state = nullptr;
        const ReportProviderSeriesReadStats *read_stats = nullptr;
        uint32_t interval_ms = 0;
    };

    PlotSeriesContext ctx;
    ctx.manager = this;
    ctx.chunk = &chunk;
    ctx.state = &plot_series_states_[chunk.stream_index];
    ctx.interval_ms =
        infer_chunk_interval_ms(chunk.record_count, chunk.start_ms, chunk.end_ms);

    ReportProviderSeriesReadStats read_stats;
    ctx.read_stats = &read_stats;

    const bool ok = for_each_result_series_sample(
        chunk,
        chunk.stream_index,
        read_stats,
        [](void *context, const ReportSeriesSample &sample) -> bool {
            PlotSeriesContext *ctx =
                static_cast<PlotSeriesContext *>(context);
            ReportManager *manager = ctx ? ctx->manager : nullptr;
            const ReportResultChunk *chunk = ctx ? ctx->chunk : nullptr;
            PlotSeriesBuildState *state = ctx ? ctx->state : nullptr;
            if (!manager || !chunk || !state) return false;

            const uint32_t interval_ms =
                (ctx->read_stats && ctx->read_stats->interval_ms)
                    ? ctx->read_stats->interval_ms
                    : ctx->interval_ms;

            return manager->process_plot_series_sample_value(*state,
                                                             *chunk,
                                                             sample,
                                                             interval_ms);
        },
        &ctx);
    if (!ok) {
        fail_result_prepare("plot_series_decode_failed");
        return false;
    }

    (void)read_stats;
    return true;
}

bool ReportManager::finish_plot_series(size_t stream_index) {
    if (stream_index >= result_stream_count_ ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        return true;
    }

    const ReportResultStream &stream = result_streams_[stream_index];
    PlotSeriesBuildState &state = plot_series_states_[stream_index];
    if (stream.kind != ReportStoreChunkKind::Series || !state.open) {
        state.reset();
        return true;
    }

    flush_plot_bucket(state);
    if (state.points.size()) {
        const int64_t bucket_ms =
            state.series_bucket_ms > 0 ? state.series_bucket_ms
                                       : plot_bucket_ms_;
        if (!append_plot_series_envelope_runs(plot_build_bin_,
                                              stream.name,
                                              state.points,
                                              bucket_ms,
                                              plot_bin_ok_)) {
            fail_result_prepare("plot_overflow");
            return false;
        }
    }

    state.reset();
    return true;
}

}  // namespace aircannect
