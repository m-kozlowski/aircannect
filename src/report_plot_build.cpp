#include "report_manager.h"

#include <algorithm>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "debug_log.h"
#include "report_event_dedupe.h"
#include "report_plot_payload.h"
#include "report_records.h"
#include "report_store.h"

namespace aircannect {

void ReportManager::reset_plot_build() {
    plot_build_active_ = false;
    plot_build_idle_prebuild_ = false;
    plot_build_night_start_ms_.store(0);
    plot_build_phase_ = ReportPlotBuildPhase::Idle;
    plot_build_bin_.clear();
    plot_tmp_.clear();
    plot_bin_ok_ = true;
    memset(plot_ranges_, 0, sizeof(plot_ranges_));
    plot_range_count_ = 0;
    plot_start_ms_ = 0;
    plot_end_ms_ = 0;
    plot_bucket_ms_ = 1;
    plot_chunk_index_ = 0;
    memset(plot_chunk_done_, 0, sizeof(plot_chunk_done_));
    plot_seen_events_.clear();
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        plot_series_states_[i].reset();
    }
    plot_build_started_ms_ = 0;
    plot_build_input_chunks_ = 0;
    plot_build_input_bytes_ = 0;
}

void ReportManager::build_empty_plot_bin(ReportSpoolBuffer &out) const {
    out.clear();
    out.set_max_size(32);
    bin_put_u32(out, PLOT_BIN_MAGIC);
    bin_put_u16(out, PLOT_BIN_VERSION);
    bin_put_u16(out, 0);   // flags
    bin_put_i64(out, 0);   // base_ms
    bin_put_u32(out, 0);   // event count; no series follow
}

int ReportManager::plot_range_index(int64_t timestamp_ms) const {
    for (size_t i = 0; i < plot_range_count_; ++i) {
        if (timestamp_ms >= plot_ranges_[i].start_ms &&
            timestamp_ms < plot_ranges_[i].end_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ReportManager::start_result_plot_build() {
    reset_plot_build();
    build_empty_plot_bin(result_plot_bin_);
    if (result_status_.state == ReportResultState::Error ||
        !result_chunks_ || result_status_.chunk_count == 0) {
        build_empty_plot_bin(result_plot_bin_);
        return publish_result_to_slot();
    }

    const size_t range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    if (range_count == 0) {
        build_empty_plot_bin(result_plot_bin_);
        return publish_result_to_slot();
    }

    plot_range_count_ = range_count;
    plot_start_ms_ = result_ranges_[0].start_ms;
    plot_end_ms_ = result_ranges_[0].end_ms;
    for (size_t i = 0; i < range_count; ++i) {
        plot_ranges_[i] = result_ranges_[i];
        plot_start_ms_ = std::min(plot_start_ms_, result_ranges_[i].start_ms);
        plot_end_ms_ = std::max(plot_end_ms_, result_ranges_[i].end_ms);
    }
    if (plot_start_ms_ <= 0 || plot_end_ms_ <= plot_start_ms_) {
        build_empty_plot_bin(result_plot_bin_);
        return publish_result_to_slot();
    }
    if (!result_skip_plot_cache_ && load_result_plot_cache()) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result plot cache hit index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long>(result_plot_bin_.size()));
        return publish_result_to_slot();
    }
    if (result_skip_plot_cache_) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result plot cache skipped index=%lu reason=refresh\n",
                  static_cast<unsigned long>(result_status_.therapy_index));
    }

    const int64_t span_ms = plot_end_ms_ - plot_start_ms_;
    plot_bucket_ms_ = std::max<int64_t>(
        1, span_ms / static_cast<int64_t>(AC_REPORT_PLOT_BUCKETS));
    plot_build_bin_.clear();
    plot_build_bin_.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    plot_tmp_.clear();
    plot_tmp_.set_max_size(128 * 1024);
    plot_bin_ok_ = true;
    if (!plot_build_bin_.reserve_capacity(AC_REPORT_PLOT_INITIAL_RESERVE)) {
        fail_result_prepare("plot_alloc_failed");
        return false;
    }
    plot_bin_ok_ &= bin_put_u32(plot_build_bin_, PLOT_BIN_MAGIC);
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_, PLOT_BIN_VERSION);
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_, 0);          // flags
    plot_bin_ok_ &= bin_put_i64(plot_build_bin_, plot_start_ms_);  // base_ms
    if (!plot_bin_ok_) {
        fail_result_prepare("plot_alloc_failed");
        return false;
    }
    plot_build_active_ = true;
    plot_build_idle_prebuild_ = active_build_idle_prebuild_;
    plot_build_started_ms_ = millis();
    plot_build_input_chunks_ = 0;
    plot_build_input_bytes_ = 0;
    plot_build_night_start_ms_.store(result_night_.start_ms);
    plot_build_phase_ = ReportPlotBuildPhase::Events;
    return true;
}

bool ReportManager::process_plot_event_chunk(const ReportResultChunk &chunk) {
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!read_result_chunk_payload(chunk, meta, payload)) {
        fail_result_prepare("plot_event_read_failed");
        return false;
    }
    const size_t count = payload.size() / report_event_record_wire_size();
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        bool in_range = false;
        for (size_t i = 0; i < plot_range_count_; ++i) {
            if (report_event_overlaps_window(
                    event,
                    plot_ranges_[i].start_ms,
                    plot_ranges_[i].end_ms,
                    AC_REPORT_EVENT_EDGE_TOLERANCE_MS)) {
                in_range = true;
                break;
            }
        }
        if (!in_range) continue;

        if (report_event_seen(plot_seen_events_, event)) continue;
        if (!remember_report_event(plot_seen_events_, event)) {
            fail_result_prepare("plot_event_dedupe_failed");
            return false;
        }
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(event.start_ms - plot_start_ms_));
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(event.duration_ms));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_, static_cast<int32_t>(event.code));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_,
                                    static_cast<int32_t>(event.flags));
    }
    return true;
}

bool ReportManager::finish_result_plot_build() {
    if (!plot_bin_ok_ || plot_build_bin_.size() == 0) {
        fail_result_prepare("plot_overflow");
        return false;
    }
    const size_t len = plot_build_bin_.size();
    result_plot_bin_.clear();
    result_plot_bin_.set_max_size(len);
    if (!result_plot_bin_.reserve_capacity(len) ||
        !result_plot_bin_.append(plot_build_bin_.data(), len)) {
        fail_result_prepare("plot_publish_failed");
        return false;
    }
    result_status_.state = report_result_settled_state(result_status_.missing_required);
    result_status_.error.clear();
    if (!publish_result_to_slot(true)) {
        reset_plot_build();
        release_result_edf_sessions();
        return false;
    }
    const uint32_t elapsed_ms =
        plot_build_started_ms_ ? static_cast<uint32_t>(millis() -
                                                       plot_build_started_ms_)
                               : 0;
    const uint32_t input_chunks = plot_build_input_chunks_;
    const uint32_t input_bytes = plot_build_input_bytes_;
    reset_plot_build();
    release_result_edf_sessions();
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Result plot ready index=%lu chunks=%lu input_chunks=%lu "
              "input_bytes=%lu bytes=%lu elapsed_ms=%lu\n",
              static_cast<unsigned long>(result_status_.therapy_index),
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(input_chunks),
              static_cast<unsigned long>(input_bytes),
              static_cast<unsigned long>(result_plot_bin_.size()),
              static_cast<unsigned long>(elapsed_ms));
    return true;
}


}  // namespace aircannect
