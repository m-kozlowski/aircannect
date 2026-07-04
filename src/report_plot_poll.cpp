#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include <Arduino.h>

#include "debug_log.h"
#include "report_plot_payload.h"

namespace aircannect {

void ReportManager::service_range_plot(bool realtime_active) {
    if (!result_slots_lock_) return;

    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    const bool active = range_req_active_;
    const size_t index = range_req_index_;
    const uint64_t night_start_ms = range_req_night_start_ms_;
    const int64_t from_ms = range_req_from_;
    const int64_t to_ms = range_req_to_;
    xSemaphoreGive(result_slots_lock_);

    if (!active) return;
    if (realtime_active) return;
    if (summary_fetch_active_) return;

    if (plot_build_active_) {
        const uint32_t elapsed_ms =
            plot_build_started_ms_
                ? static_cast<uint32_t>(millis() - plot_build_started_ms_)
                : 0;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Range plot preempted overview index=%lu "
                  "night=%llu elapsed_ms=%lu\n",
                  static_cast<unsigned long>(index),
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(elapsed_ms));
        reset_plot_build();
        release_result_edf_sessions();
    }

    // Yield an idle prefetch so the range builds now; a real foreground fetch is
    // not yielded, so wait for that.
    if (cache_fetch_active_) {
        prefetch_yield_to_foreground();
        if (cache_fetch_active_) return;
    }

    if (range_build_active_) {
        if (range_build_index_ == index &&
            range_night_start_ms_ == night_start_ms &&
            range_build_from_ == from_ms &&
            range_build_to_ == to_ms) {
            return;
        }
        reset_range_plot_build(false);
    }

    bool waiting_for_result = false;
    if (!start_range_plot_build(night_start_ms,
                                index,
                                from_ms,
                                to_ms,
                                waiting_for_result)) {
        if (waiting_for_result) return;

        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        if (range_req_active_ && range_req_index_ == index &&
            range_req_night_start_ms_ == night_start_ms &&
            range_req_from_ == from_ms && range_req_to_ == to_ms) {
            range_req_active_ = false;
        }
        xSemaphoreGive(result_slots_lock_);
    }
}

void ReportManager::poll_range_plot_build() {
    if (!range_build_active_) return;

    size_t reads = 0;
    const uint32_t started_ms = millis();

    auto budget_spent = [&]() -> bool {
        if (reads == 0) return false;
        if (reads >= AC_REPORT_RANGE_PLOT_POLL_CHUNK_CAP) return true;

        return static_cast<uint32_t>(millis() - started_ms) >=
               AC_REPORT_RANGE_PLOT_POLL_BUDGET_MS;
    };

    while (range_build_active_ && !budget_spent()) {
        // Event chunks
        if (range_build_phase_ == ReportPlotBuildPhase::Events) {
            bool processed = false;

            while (range_chunk_index_ < range_chunk_count_) {
                const ReportResultChunk &chunk =
                    range_chunks_[range_chunk_index_++];

                if (chunk.kind != ReportStoreChunkKind::Events) continue;

                if (chunk.end_ms <= range_build_from_ ||
                    chunk.start_ms >= range_build_to_) {
                    continue;
                }

                if (!process_range_event_chunk(chunk)) return;

                reads++;
                range_build_input_chunks_++;
                range_build_input_bytes_ += chunk.payload_len;
                processed = true;

                break;
            }

            if (processed) continue;

            if (!range_build_bytes_) {
                fail_range_plot_build("range_bad_state");
                return;
            }

            range_build_ok_ &=
                bin_put_u32(*range_build_bytes_, range_event_count_);

            if (range_tmp_.size()) {
                range_build_ok_ &=
                    range_build_bytes_->append(range_tmp_.data(),
                                               range_tmp_.size());
            }

            range_tmp_.clear();

            if (!range_build_ok_) {
                fail_range_plot_build("range_overflow");
                return;
            }

            range_build_phase_ = ReportPlotBuildPhase::Series;
            range_chunk_index_ = 0;
            range_stream_index_ = 0;

            continue;
        }

        // Series chunks
        if (range_build_phase_ == ReportPlotBuildPhase::Series) {
            if (range_stream_index_ >= range_stream_count_) {
                finish_range_plot_build();
                return;
            }

            const ReportResultStream &stream =
                range_streams_[range_stream_index_];

            if (stream.kind != ReportStoreChunkKind::Series ||
                !stream.name || !stream.name[0] ||
                stream.chunk_count == 0) {
                range_stream_index_++;
                range_chunk_index_ = 0;

                continue;
            }

            if (!range_series_open_ && !open_range_series(stream)) {
                fail_range_plot_build("range_series_open_failed");
                return;
            }

            bool processed = false;

            while (range_chunk_index_ < range_chunk_count_) {
                const ReportResultChunk &chunk =
                    range_chunks_[range_chunk_index_++];

                if (!result_chunk_matches_stream(chunk,
                                                 range_stream_index_,
                                                 stream)) {
                    continue;
                }

                if (chunk.end_ms <= range_build_from_ ||
                    chunk.start_ms >= range_build_to_) {
                    continue;
                }

                if (!process_range_series_chunk(chunk,
                                                range_stream_index_)) {
                    return;
                }

                reads++;
                range_build_input_chunks_++;
                range_build_input_bytes_ += chunk.payload_len;
                processed = true;

                break;
            }

            if (processed) continue;

            if (!finish_range_series()) return;

            range_stream_index_++;
            range_chunk_index_ = 0;

            continue;
        }

        fail_range_plot_build("range_bad_state");
        return;
    }
}

void ReportManager::poll_result_plot_build() {
    if (!plot_build_active_) return;

    size_t reads = 0;
    const uint32_t started_ms = millis();

    auto budget_spent = [&]() -> bool {
        if (reads == 0) return false;
        if (reads >= AC_REPORT_PLOT_POLL_CHUNK_CAP) return true;

        return static_cast<uint32_t>(millis() - started_ms) >=
               AC_REPORT_PLOT_POLL_BUDGET_MS;
    };

    while (plot_build_active_ && !budget_spent()) {
        // Event chunks
        if (plot_build_phase_ == ReportPlotBuildPhase::Events) {
            bool processed = false;

            while (plot_chunk_index_ < result_status_.chunk_count) {
                const ReportResultChunk &chunk =
                    result_chunks_[plot_chunk_index_++];

                if (chunk.kind != ReportStoreChunkKind::Events) continue;

                if (!process_plot_event_chunk(chunk)) return;

                processed = true;
                reads++;
                plot_build_input_chunks_++;
                plot_build_input_bytes_ += chunk.payload_len;

                break;
            }

            if (processed) continue;

            // Event phase footer
            const uint32_t event_count =
                static_cast<uint32_t>(plot_tmp_.size() / 16);

            plot_bin_ok_ &= bin_put_u32(plot_build_bin_, event_count);

            if (plot_tmp_.size()) {
                plot_bin_ok_ &=
                    plot_build_bin_.append(plot_tmp_.data(), plot_tmp_.size());
            }

            plot_tmp_.clear();
            plot_build_phase_ = ReportPlotBuildPhase::Series;
            plot_chunk_index_ = 0;
            memset(plot_chunk_done_, 0, sizeof(plot_chunk_done_));

            continue;
        }

        // Series chunks
        if (plot_build_phase_ == ReportPlotBuildPhase::Series) {
            bool processed = false;

            const size_t max_chunks = std::min(
                static_cast<size_t>(result_status_.chunk_count),
                static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));

            while (plot_chunk_index_ < max_chunks) {
                const size_t chunk_index = plot_chunk_index_++;
                if (plot_chunk_done_[chunk_index]) continue;

                const ReportResultChunk &chunk = result_chunks_[chunk_index];

                if (chunk.kind != ReportStoreChunkKind::Series) {
                    plot_chunk_done_[chunk_index] = true;
                    continue;
                }

                if (chunk.stream_index >= result_stream_count_ ||
                    chunk.stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
                    plot_chunk_done_[chunk_index] = true;
                    continue;
                }

                if (chunk.provider_ref.provider == ReportProviderId::Edf) {
                    if (!process_plot_edf_series_batch(chunk_index,
                                                       processed)) {
                        return;
                    }

                    if (processed) {
                        reads++;
                        break;
                    }

                    continue;
                }

                if (!process_plot_series_chunk(chunk_index)) return;

                plot_chunk_done_[chunk_index] = true;
                plot_build_input_chunks_++;
                plot_build_input_bytes_ += chunk.payload_len;
                processed = true;
                reads++;

                break;
            }

            if (processed) continue;

            for (size_t i = 0; i < result_stream_count_ &&
                               i < AC_REPORT_RESULT_STREAM_MAX;
                 ++i) {
                if (!finish_plot_series(i)) return;
            }

            if (!plot_bin_ok_) {
                fail_result_prepare("plot_overflow");
                return;
            }

            if (plot_chunk_index_ >= max_chunks) {
                finish_result_plot_build();
                return;
            }

            continue;
        }

        fail_result_prepare("plot_bad_state");
        return;
    }
}

}  // namespace aircannect
