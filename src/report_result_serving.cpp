#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include "debug_log.h"
#include "report_index_scratch.h"
#include "report_night_index.h"
#include "string_util.h"

namespace aircannect {
namespace {

const char *build_queue_result_name(uint8_t result) {
    switch (result) {
        case 0:
            return "queued";
        case 1:
            return "already";
        case 2:
            return "full";
        case 3:
            return "unavailable";
    }
    return "unknown";
}

bool result_state_http_cacheable(ReportResultState state) {
    return state == ReportResultState::Ready ||
           state == ReportResultState::Partial;
}

bool result_state_materialized_slot_allowed(ReportResultState state) {
    return state != ReportResultState::Preparing;
}

}  // namespace

ReportManager::ResultRead ReportManager::read_result(
    size_t therapy_index,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    ScopedIndexedNight indexed_night("read_result_index");
    if (!indexed_night ||
        !indexed_night_by_therapy_index(therapy_index, indexed_night.get())) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "not_found");
            xSemaphoreGive(build_queue_lock_);
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::NotFound;
    }

    return read_result_for_indexed_night(therapy_index,
                                         indexed_night.get(),
                                         if_none_match,
                                         etag_out,
                                         etag_out_size,
                                         json_out);
}

ReportManager::ResultRead ReportManager::read_result_by_start(
    uint64_t night_start_ms,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    ScopedIndexedNight indexed_night("read_result_start_index");
    size_t therapy_index = 0;
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms,
                                indexed_night.get(),
                                &therapy_index)) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "not_found");
            xSemaphoreGive(build_queue_lock_);
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::NotFound;
    }

    return read_result_for_indexed_night(therapy_index,
                                         indexed_night.get(),
                                         if_none_match,
                                         etag_out,
                                         etag_out_size,
                                         json_out);
}

ReportManager::ResultRead ReportManager::read_result_for_indexed_night(
    size_t therapy_index,
    const ReportIndexedNight &indexed_night,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "summary_busy");
            xSemaphoreGive(build_queue_lock_);
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::Busy;
    }

    format_night_etag_unlocked(indexed_night.summary,
                               indexed_night.source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();
    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }

    if (indexed_night.edf_catalog_pending) {
        if (load_result_json_cache_for_night(indexed_night,
                                             current_etag,
                                             json_out)) {
            const bool not_modified = if_none_match && if_none_match[0] &&
                                      strcmp(if_none_match, current_etag) == 0;
            if (build_queue_lock_ &&
                xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) ==
                    pdTRUE) {
                copy_cstr(build_queue_last_read_,
                          sizeof(build_queue_last_read_),
                          not_modified
                              ? "not_modified_sd_pending"
                              : "ready_sd_pending");
                xSemaphoreGive(build_queue_lock_);
            }

            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result cache pending-catalog hit index=%lu "
                      "night=%llu etag=%s bytes=%lu\n",
                      static_cast<unsigned long>(therapy_index),
                      static_cast<unsigned long long>(
                          indexed_night.summary.start_ms),
                      current_etag,
                      static_cast<unsigned long>(json_out.length()));
            if (not_modified) {
                json_out.clear();
                return ResultRead::NotModified;
            }
            return ResultRead::Ready;
        }

        if (etag_out && etag_out_size) etag_out[0] = '\0';
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "edf_catalog");
            xSemaphoreGive(build_queue_lock_);
        }
        return ResultRead::Building;
    }

    ResultRead slot_read = ResultRead::NotFound;
    bool have_slot_read = false;
    if (result_slots_ && result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (result_slots_[i].valid &&
                result_slots_[i].night_start_ms ==
                    indexed_night.summary.start_ms) {
                if (strcmp(result_slots_[i].etag, current_etag) != 0) {
                    clear_materialized_slot_locked(result_slots_[i]);
                    clear_range_plot_locked(indexed_night.summary.start_ms,
                                            false);
                    update_materialized_status_locked();
                    continue;
                }

                if (!result_state_materialized_slot_allowed(
                        result_slots_[i].status.state)) {
                    clear_materialized_slot_locked(result_slots_[i]);
                    clear_range_plot_locked(indexed_night.summary.start_ms,
                                            false);
                    update_materialized_status_locked();
                    continue;
                }

                result_slots_[i].last_used = ++result_slot_tick_;
                snprintf(etag, sizeof(etag), "%s", result_slots_[i].etag);
                const ReportResultStatus &status = result_slots_[i].status;
                const bool cacheable =
                    result_state_http_cacheable(status.state);
                if (etag_out && etag_out_size) {
                    snprintf(etag_out,
                             etag_out_size,
                             "%s",
                             cacheable ? etag : "");
                }

                if (cacheable && if_none_match && if_none_match[0] &&
                    strcmp(if_none_match, etag) == 0) {
                    slot_read = ResultRead::NotModified;
                } else {
                    const ReportCacheFetchStatus inactive{};
                    build_result_json_from(
                        status,
                        result_slots_[i].night,
                        result_slots_[i].ranges,
                        std::min(result_slots_[i].range_count,
                                 static_cast<size_t>(
                                     AC_REPORT_SUMMARY_SESSION_MAX)),
                        result_slots_[i].streams,
                        std::min(result_slots_[i].stream_count,
                                 static_cast<size_t>(
                                     AC_REPORT_RESULT_STREAM_MAX)),
                        inactive,
                        json_out);
                    slot_read = ResultRead::Ready;
                }

                have_slot_read = true;
                break;
            }
        }
        xSemaphoreGive(result_slots_lock_);
    }
    if (have_slot_read) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      slot_read == ResultRead::NotModified ? "not_modified"
                                                           : "ready");
            xSemaphoreGive(build_queue_lock_);
        }
        return slot_read;
    }

    if (load_result_json_cache_for_night(indexed_night,
                                         current_etag,
                                         json_out)) {
        const bool not_modified = if_none_match && if_none_match[0] &&
                                  strcmp(if_none_match, current_etag) == 0;
        if (etag_out && etag_out_size) {
            snprintf(etag_out, etag_out_size, "%s", current_etag);
        }
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      not_modified ? "not_modified_sd" : "ready_sd");
            xSemaphoreGive(build_queue_lock_);
        }

        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result cache direct hit index=%lu night=%llu bytes=%lu\n",
                  static_cast<unsigned long>(therapy_index),
                  static_cast<unsigned long long>(
                      indexed_night.summary.start_ms),
                  static_cast<unsigned long>(json_out.length()));
        if (not_modified) {
            json_out.clear();
            return ResultRead::NotModified;
        }
        return ResultRead::Ready;
    }

    if (etag_out && etag_out_size) etag_out[0] = '\0';

    const BuildQueueResult queued =
        enqueue_build(indexed_night.summary.start_ms,
                      therapy_index,
                      false);
    if (build_queue_lock_ &&
        xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
        copy_cstr(build_queue_last_read_,
                  sizeof(build_queue_last_read_),
                  build_queue_result_name(static_cast<uint8_t>(queued)));
        xSemaphoreGive(build_queue_lock_);
    }

    switch (queued) {
        case BuildQueueResult::Queued:
        case BuildQueueResult::AlreadyQueued:
            return ResultRead::Building;
        case BuildQueueResult::Full:
            return ResultRead::QueueFull;
        case BuildQueueResult::Unavailable:
        default:
            return ResultRead::Unavailable;
    }
}

}  // namespace aircannect
