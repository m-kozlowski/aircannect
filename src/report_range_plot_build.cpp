#include "report_manager.h"

#include <limits.h>
#include <stdint.h>

#include <Arduino.h>

#include "debug_log.h"
#include "report_plot_payload.h"

namespace aircannect {

bool ReportManager::process_range_series_sample_value(
    const ReportSeriesSample &sample,
    ReportSignalId signal,
    ReportSourceId source,
    uint32_t interval_ms,
    int32_t scale,
    bool &capped,
    bool &overflow) {
    if (sample.timestamp_ms < range_build_from_ ||
        sample.timestamp_ms >= range_build_to_) {
        return true;
    }
    const int sample_range_index = range_plot_range_index(sample.timestamp_ms);
    if (sample_range_index < 0) return true;
    if (range_have_last_sample_ &&
        (sample_range_index != range_last_range_index_ ||
         sample.timestamp_ms >
             range_last_sample_ms_ + plot_gap_threshold_ms(interval_ms))) {
        range_series_points_ += emit_plot_gap_to(range_tmp_,
                                                 range_bucket_,
                                                 range_build_from_,
                                                 range_build_ok_);
        range_current_bucket_ = -1;
        if (!range_build_ok_) {
            overflow = true;
            return false;
        }
        if (range_series_points_ >= AC_REPORT_RANGE_MAX_POINTS) {
            range_chunk_index_ = static_cast<uint32_t>(range_chunk_count_);
            capped = true;
            return false;
        }
    }
    const int64_t bucket_ms =
        plot_bucket_ms_for_signal(signal,
                                  source,
                                  range_bucket_ms_,
                                  interval_ms,
                                  true);
    int64_t sample_bucket =
        (sample.timestamp_ms - range_build_from_) / bucket_ms;
    if (sample_bucket < 0) sample_bucket = 0;
    if (range_current_bucket_ != sample_bucket) {
        range_series_points_ += flush_plot_bucket_to(range_tmp_,
                                                     range_bucket_,
                                                     range_build_from_,
                                                     range_build_ok_);
        if (!range_build_ok_) {
            overflow = true;
            return false;
        }
        range_current_bucket_ = sample_bucket;
        if (range_series_points_ >= AC_REPORT_RANGE_MAX_POINTS) {
            range_chunk_index_ = static_cast<uint32_t>(range_chunk_count_);
            capped = true;
            return false;
        }
    }
    int64_t value = static_cast<int64_t>(sample.value_milli) * scale;
    if (value > INT32_MAX) value = INT32_MAX;
    else if (value < INT32_MIN) value = INT32_MIN;
    const int32_t value_i32 = static_cast<int32_t>(value);
    if (!range_bucket_.have) {
        range_bucket_.have = true;
        range_bucket_.start_t = sample.timestamp_ms;
        range_bucket_.end_t = sample.timestamp_ms;
        range_bucket_.min_t = sample.timestamp_ms;
        range_bucket_.max_t = sample.timestamp_ms;
        range_bucket_.start_value = value_i32;
        range_bucket_.end_value = value_i32;
        range_bucket_.min_value = value_i32;
        range_bucket_.max_value = value_i32;
    } else {
        range_bucket_.end_t = sample.timestamp_ms;
        range_bucket_.end_value = value_i32;
        if (value_i32 < range_bucket_.min_value) {
            range_bucket_.min_value = value_i32;
            range_bucket_.min_t = sample.timestamp_ms;
        }
        if (value_i32 > range_bucket_.max_value) {
            range_bucket_.max_value = value_i32;
            range_bucket_.max_t = sample.timestamp_ms;
        }
    }
    range_have_last_sample_ = true;
    range_last_sample_ms_ = sample.timestamp_ms;
    range_last_range_index_ = sample_range_index;
    return true;
}

bool ReportManager::process_range_series_chunk(
    const ReportResultChunk &chunk) {
    return process_range_series_chunk(chunk, chunk.stream_index);
}

bool ReportManager::process_range_series_chunk(
    const ReportResultChunk &chunk,
    size_t stream_index) {
    if (stream_index >= range_stream_count_) {
        fail_range_plot_build("range_bad_stream");
        return false;
    }
    ReportProviderChunk provider_chunk;
    if (!provider_chunk_from_result_stream(chunk,
                                           stream_index,
                                           range_streams_,
                                           range_stream_count_,
                                           range_edf_sessions_,
                                           range_edf_session_count_,
                                           provider_chunk)) {
        fail_range_plot_build("range_chunk_map_failed");
        return false;
    }
    const int32_t scale =
        (provider_chunk.source == ReportSourceId::RespiratoryFlow6p25Hz ||
         provider_chunk.source == ReportSourceId::Leak0p5Hz)
            ? 60
            : 1;

    struct RangeSeriesContext {
        ReportManager *manager = nullptr;
        const ReportProviderSeriesReadStats *read_stats = nullptr;
        ReportSignalId signal = ReportSignalId::Flow;
        ReportSourceId source = ReportSourceId::Summary;
        uint32_t interval_ms = 0;
        int32_t scale = 1;
        bool capped = false;
        bool overflow = false;
    };
    RangeSeriesContext ctx;
    ctx.manager = this;
    ctx.signal = provider_chunk.signal;
    ctx.source = provider_chunk.source;
    ctx.scale = scale;
    ctx.interval_ms =
        infer_chunk_interval_ms(provider_chunk.record_count,
                                provider_chunk.start_ms,
                                provider_chunk.end_ms);
    ReportProviderSeriesReadStats read_stats;
    ctx.read_stats = &read_stats;
    const bool ok = for_each_range_series_sample(
        chunk,
        stream_index,
        read_stats,
        [](void *context, const ReportSeriesSample &sample) -> bool {
            RangeSeriesContext *ctx =
                static_cast<RangeSeriesContext *>(context);
            ReportManager *manager = ctx ? ctx->manager : nullptr;
            if (!manager) return false;
            const uint32_t interval_ms =
                (ctx->read_stats && ctx->read_stats->interval_ms)
                    ? ctx->read_stats->interval_ms
                    : ctx->interval_ms;
            return manager->process_range_series_sample_value(sample,
                                                              ctx->signal,
                                                              ctx->source,
                                                              interval_ms,
                                                              ctx->scale,
                                                              ctx->capped,
                                                              ctx->overflow);
        },
        &ctx);
    if (!ok && !ctx.capped) {
        fail_range_plot_build(ctx.overflow ? "range_overflow"
                                           : "range_series_decode_failed");
        return false;
    }
    (void)read_stats;
    return true;
}

bool ReportManager::finish_range_series() {
    if (!range_build_bytes_ || !range_series_open_) return false;
    range_series_points_ += flush_plot_bucket_to(range_tmp_,
                                                 range_bucket_,
                                                 range_build_from_,
                                                 range_build_ok_);
    if (range_series_points_ > 0) {
        const ReportResultStream &stream =
            range_streams_[range_stream_index_];
        if (!append_plot_series_compact(*range_build_bytes_,
                                        stream.name,
                                        range_tmp_,
                                        range_build_ok_)) {
            fail_range_plot_build("range_overflow");
            return false;
        }
    }
    range_tmp_.clear();
    range_series_open_ = false;
    range_series_points_ = 0;
    range_current_bucket_ = -1;
    range_have_last_sample_ = false;
    range_last_sample_ms_ = 0;
    if (!range_build_ok_) {
        fail_range_plot_build("range_overflow");
        return false;
    }
    return true;
}

void ReportManager::finish_range_plot_build() {
    if (!range_build_bytes_) {
        fail_range_plot_build("range_bad_state");
        return;
    }
    const PlotBlobScan scan = scan_plot_blob(*range_build_bytes_);
    if (!scan.valid) {
        fail_range_plot_build("range_invalid_blob");
        return;
    }

    if (result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        if (range_req_active_ && range_req_index_ == range_build_index_ &&
            range_req_night_start_ms_ == range_night_start_ms_ &&
            range_req_from_ == range_build_from_ &&
            range_req_to_ == range_build_to_) {
            range_plot_bytes_ = range_build_bytes_;
            range_plot_index_ = range_build_index_;
            range_plot_night_start_ms_ = range_night_start_ms_;
            range_plot_from_ = range_build_from_;
            range_plot_to_ = range_build_to_;
            range_req_active_ = false;
        }
        xSemaphoreGive(result_slots_lock_);
    }
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Range plot ready index=%lu points=%lu input_chunks=%lu "
              "input_bytes=%lu bytes=%lu elapsed_ms=%lu\n",
              static_cast<unsigned long>(range_build_index_),
              static_cast<unsigned long>(scan.points),
              static_cast<unsigned long>(range_build_input_chunks_),
              static_cast<unsigned long>(range_build_input_bytes_),
              static_cast<unsigned long>(range_build_bytes_->size()),
              static_cast<unsigned long>(
                  range_build_started_ms_
                      ? static_cast<uint32_t>(millis() -
                                              range_build_started_ms_)
                      : 0));
    reset_range_plot_build(false);
}

void ReportManager::fail_range_plot_build(const char *message) {
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "Range plot failed index=%lu error=%s\n",
              static_cast<unsigned long>(range_build_index_),
              message ? message : "range_failed");
    if (result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        if (range_req_active_ && range_req_index_ == range_build_index_ &&
            range_req_night_start_ms_ == range_night_start_ms_ &&
            range_req_from_ == range_build_from_ &&
            range_req_to_ == range_build_to_) {
            range_req_active_ = false;
        }
        xSemaphoreGive(result_slots_lock_);
    }
    reset_range_plot_build(false);
}

}  // namespace aircannect
