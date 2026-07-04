#include "report_manager.h"

#include <algorithm>
#include <memory>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "report_result_json.h"

namespace aircannect {
namespace {

bool result_state_materialized_slot_allowed(ReportResultState state) {
    return state != ReportResultState::Preparing;
}

}  // namespace

bool ReportManager::publish_result_to_slot(bool cache_plot) {
    if (!ensure_result_slots()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped reason=slot_alloc_failed "
                  "index=%lu night=%llu state=%s error=%s\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long long>(
                      result_indexed_night_.summary.start_ms),
                  result_state_name(),
                  result_status_.error.length() ? result_status_.error.c_str()
                                                : "--");
        return false;
    }

    if (!result_state_materialized_slot_allowed(result_status_.state)) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result publish skipped reason=state index=%lu night=%llu "
                  "state=%s error=%s\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long long>(
                      result_indexed_night_.summary.start_ms),
                  result_state_name(),
                  result_status_.error.length() ? result_status_.error.c_str()
                                                : "--");
        return false;
    }

    if (!result_etag_[0] || result_indexed_night_.summary.start_ms == 0) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped reason=%s index=%lu night=%llu "
                  "state=%s error=%s\n",
                  !result_etag_[0] ? "missing_etag" : "missing_night",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long long>(
                      result_indexed_night_.summary.start_ms),
                  result_state_name(),
                  result_status_.error.length() ? result_status_.error.c_str()
                                                : "--");
        return false;
    }

    // Snapshot the plot bytes outside the slot lock (the copy is the heavy
    // part); a shared_ptr lets a GET stream it even after the blob is evicted.
    std::shared_ptr<ReportSpoolBuffer> plot;
    if (result_plot_bin_.size() > 0) {
        plot = std::make_shared<ReportSpoolBuffer>();
        plot->set_max_size(result_plot_bin_.size());
        if (!plot->reserve_capacity(result_plot_bin_.size()) ||
            !plot->append(result_plot_bin_.data(), result_plot_bin_.size())) {
            plot.reset();
        }
    }

    if (result_plot_bin_.size() > 0 && !plot) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped: plot snapshot failed "
                  "index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long>(result_plot_bin_.size()));
        return false;
    }

    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);

    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            result_slots_[i].night_start_ms ==
                result_indexed_night_.summary.start_ms) {
            pick = i;
            found = true;
            break;
        }
    }

    if (!found) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid) {
                pick = i;
                break;
            }
            if (result_slots_[i].last_used < result_slots_[pick].last_used) {
                pick = i;
            }
        }
    }

    MaterializedResult &slot = result_slots_[pick];
    slot.valid = true;
    slot.night_start_ms = result_indexed_night_.summary.start_ms;
    snprintf(slot.etag, sizeof(slot.etag), "%s", result_etag_);
    slot.status = result_status_;
    slot.night = result_indexed_night_;

    slot.range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        slot.ranges[i] = (i < slot.range_count) ? result_ranges_[i]
                                                : PlotRange{};
    }

    slot.stream_count = result_stream_count_;
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        slot.streams[i] = (i < result_stream_count_) ? result_streams_[i]
                                                     : ReportResultStream{};
    }

    slot.chunk_count =
        std::min(static_cast<size_t>(result_status_.chunk_count),
                 static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_CHUNK_MAX; ++i) {
        slot.chunks[i] = (result_chunks_ && i < slot.chunk_count)
                             ? result_chunks_[i]
                             : ReportResultChunk{};
    }

    slot.edf_session_count =
        std::min(result_edf_session_count_,
                 static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_EDF_SESSION_MAX; ++i) {
        slot.edf_sessions[i] =
            (result_edf_sessions_ && i < slot.edf_session_count)
                ? result_edf_sessions_[i]
                : EdfReportSessionDescriptor{};
    }

    slot.plot = plot;
    slot.last_used = ++result_slot_tick_;
    update_materialized_status_locked();
    xSemaphoreGive(result_slots_lock_);

    if (cache_plot && plot) {
        LargeTextBuffer result_json_text;
        result_json_text.reserve(8192);

        const ReportCacheFetchStatus inactive_cache{};
        build_result_json_from(result_status_,
                               result_indexed_night_,
                               result_ranges_,
                               result_range_count_,
                               result_streams_,
                               result_stream_count_,
                               inactive_cache,
                               result_json_text);

        std::shared_ptr<ReportSpoolBuffer> result_json;
        if (!result_json_text.overflowed() && result_json_text.length() > 0) {
            result_json = std::make_shared<ReportSpoolBuffer>();
            if (result_json) {
                result_json->set_max_size(result_json_text.length());
                if (!result_json->reserve_capacity(result_json_text.length()) ||
                    !result_json->append(
                        reinterpret_cast<const uint8_t *>(
                            result_json_text.c_str()),
                        result_json_text.length())) {
                    result_json.reset();
                }
            }
        }

        if (result_json) {
            enqueue_result_cache_write(result_indexed_night_,
                                       result_etag_,
                                       result_json,
                                       plot);
        } else {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "Result cache write skipped: result JSON snapshot "
                      "failed index=%lu night=%llu\n",
                      static_cast<unsigned long>(
                          result_status_.therapy_index),
                      static_cast<unsigned long long>(
                          result_indexed_night_.summary.start_ms));
        }
    }

    return true;
}

void ReportManager::update_materialized_status_locked() {
    uint32_t slots = 0;
    uint32_t plot_slots = 0;
    if (result_slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid) continue;
            slots++;
            if (result_slots_[i].plot && result_slots_[i].plot->size() > 0) {
                plot_slots++;
            }
        }
    }

    result_status_.materialized_slots = slots;
    result_status_.materialized_plot_slots = plot_slots;
}

void ReportManager::clear_materialized_slot_locked(MaterializedResult &slot) {
    slot.valid = false;
    slot.night_start_ms = 0;
    slot.etag[0] = '\0';
    slot.status = ReportResultStatus{};
    memset(&slot.night, 0, sizeof(slot.night));
    memset(slot.ranges, 0, sizeof(slot.ranges));
    slot.range_count = 0;
    memset(slot.streams, 0, sizeof(slot.streams));
    slot.stream_count = 0;
    memset(slot.chunks, 0, sizeof(slot.chunks));
    slot.chunk_count = 0;
    memset(slot.edf_sessions, 0, sizeof(slot.edf_sessions));
    slot.edf_session_count = 0;
    slot.plot.reset();
}

void ReportManager::clear_range_plot_locked(uint64_t night_start_ms, bool all) {
    const bool matches_request =
        range_req_active_ &&
        (all || range_req_night_start_ms_ == night_start_ms);
    const bool matches_plot =
        range_plot_bytes_ &&
        (all || range_plot_night_start_ms_ == night_start_ms);
    if (!matches_request && !matches_plot) return;

    if (matches_request) {
        range_req_active_ = false;
        range_req_index_ = 0;
        range_req_night_start_ms_ = 0;
        range_req_from_ = 0;
        range_req_to_ = 0;
    }

    if (matches_plot) {
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }
}

void ReportManager::invalidate_materialized_locked(uint64_t night_start_ms,
                                                   bool all) {
    if (!result_slots_) return;

    bool invalidated = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            (all || result_slots_[i].night_start_ms == night_start_ms)) {
            clear_materialized_slot_locked(result_slots_[i]);
            invalidated = true;
        }
    }

    if (invalidated) {
        clear_range_plot_locked(night_start_ms, all);
    }
    update_materialized_status_locked();
}

void ReportManager::invalidate_materialized(uint64_t night_start_ms, bool all) {
    if (!result_slots_lock_) return;

    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    invalidate_materialized_locked(night_start_ms, all);
    xSemaphoreGive(result_slots_lock_);
}

}  // namespace aircannect
