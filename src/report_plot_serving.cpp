#include "report_manager.h"

#include <stdlib.h>
#include <string.h>

#include "debug_log.h"
#include "report_index_scratch.h"
#include "report_night_index.h"
#include "report_plot_payload.h"

namespace aircannect {
namespace {

bool parse_report_night_start_from_etag(const char *etag,
                                        uint64_t &night_start_ms) {
    if (!etag || !etag[0]) return false;

    char *end = nullptr;
    const unsigned long long parsed = strtoull(etag, &end, 10);
    if (end == etag || !end || *end != '-') return false;

    night_start_ms = static_cast<uint64_t>(parsed);
    return night_start_ms != 0;
}

}  // namespace

ReportManager::PlotRead ReportManager::read_plot(
    size_t therapy_index, const char *version,
    char *etag_out, size_t etag_out_size,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    ScopedIndexedNight indexed_night("read_plot_index");
    if (!indexed_night) return PlotRead::Unavailable;

    size_t resolved_therapy_index = therapy_index;
    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (etag_out && etag_out_size) etag_out[0] = '\0';

    uint64_t version_night_start_ms = 0;
    const bool have_version_start =
        parse_report_night_start_from_etag(version, version_night_start_ms);
    const bool found_night = have_version_start
        ? indexed_night_by_start(version_night_start_ms,
                                 indexed_night.get(),
                                 &resolved_therapy_index)
        : indexed_night_by_therapy_index(therapy_index, indexed_night.get());
    if (!found_night) return PlotRead::NotFound;

    if (!take_summary_lock(pdMS_TO_TICKS(20))) return PlotRead::Busy;
    format_night_etag_unlocked(indexed_night->summary,
                               indexed_night->source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();

    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }
    if (version && version[0] && strcmp(version, current_etag) != 0) {
        return PlotRead::Stale;
    }

    if (indexed_night->edf_catalog_pending) {
        auto cached_plot = std::make_shared<ReportSpoolBuffer>();
        if (cached_plot &&
            load_result_plot_cache_for_night(indexed_night.get(),
                                             current_etag,
                                             *cached_plot)) {
            out = cached_plot;
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Plot cache pending-catalog hit index=%lu "
                      "night=%llu bytes=%lu\n",
                      static_cast<unsigned long>(resolved_therapy_index),
                      static_cast<unsigned long long>(
                          indexed_night->summary.start_ms),
                      static_cast<unsigned long>(cached_plot->size()));
            return PlotRead::Ready;
        }

        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return PlotRead::Building;
    }

    bool matching_result_without_plot = false;
    if (result_slots_ && result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid ||
                result_slots_[i].night_start_ms !=
                    indexed_night->summary.start_ms) {
                continue;
            }
            if (strcmp(result_slots_[i].etag, current_etag) != 0) {
                clear_materialized_slot_locked(result_slots_[i]);
                update_materialized_status_locked();
                continue;
            }

            result_slots_[i].last_used = ++result_slot_tick_;
            if (result_slots_[i].status.state == ReportResultState::Error) {
                xSemaphoreGive(result_slots_lock_);
                return PlotRead::Error;
            }
            if (result_slots_[i].plot) {
                out = result_slots_[i].plot;
                xSemaphoreGive(result_slots_lock_);
                return PlotRead::Ready;
            }

            matching_result_without_plot = true;
            break;
        }
        xSemaphoreGive(result_slots_lock_);
    }

    auto cached_plot = std::make_shared<ReportSpoolBuffer>();
    if (cached_plot &&
        load_result_plot_cache_for_night(indexed_night.get(),
                                         current_etag,
                                         *cached_plot)) {
        if (result_slots_ && result_slots_lock_) {
            xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
            for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
                if (!result_slots_[i].valid ||
                    result_slots_[i].night_start_ms !=
                        indexed_night->summary.start_ms) {
                    continue;
                }
                if (strcmp(result_slots_[i].etag, current_etag) != 0) {
                    clear_materialized_slot_locked(result_slots_[i]);
                    update_materialized_status_locked();
                    continue;
                }

                result_slots_[i].plot = cached_plot;
                result_slots_[i].last_used = ++result_slot_tick_;
                update_materialized_status_locked();
                break;
            }
            xSemaphoreGive(result_slots_lock_);
        }

        out = cached_plot;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Plot cache direct hit index=%lu night=%llu bytes=%lu\n",
                  static_cast<unsigned long>(resolved_therapy_index),
                  static_cast<unsigned long long>(
                      indexed_night->summary.start_ms),
                  static_cast<unsigned long>(cached_plot->size()));
        return PlotRead::Ready;
    }

    if (matching_result_without_plot &&
        plot_build_night_start_ms_.load() == indexed_night->summary.start_ms) {
        return PlotRead::Building;
    }

    switch (enqueue_build(indexed_night->summary.start_ms,
                          resolved_therapy_index,
                          false)) {
        case BuildQueueResult::Queued:
        case BuildQueueResult::AlreadyQueued:
            return PlotRead::Building;
        case BuildQueueResult::Full:
            return PlotRead::QueueFull;
        case BuildQueueResult::Unavailable:
        default:
            return PlotRead::Unavailable;
    }
}

ReportManager::PlotRead ReportManager::read_plot_range(
    size_t therapy_index, const char *version,
    char *etag_out, size_t etag_out_size,
    int64_t from_ms, int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (etag_out && etag_out_size) etag_out[0] = '\0';
    if (to_ms <= from_ms) return PlotRead::NotFound;
    if (!ensure_result_slots()) return PlotRead::Unavailable;

    ScopedIndexedNight indexed_night("read_plot_range_index");
    if (!indexed_night) return PlotRead::Unavailable;

    size_t resolved_therapy_index = therapy_index;
    uint64_t version_night_start_ms = 0;
    const bool have_version_start =
        parse_report_night_start_from_etag(version, version_night_start_ms);
    const bool found_night = have_version_start
        ? indexed_night_by_start(version_night_start_ms,
                                 indexed_night.get(),
                                 &resolved_therapy_index)
        : indexed_night_by_therapy_index(therapy_index, indexed_night.get());
    if (!found_night) return PlotRead::NotFound;

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return PlotRead::Busy;
    format_night_etag_unlocked(indexed_night->summary,
                               indexed_night->source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();

    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }
    if (version && version[0] && strcmp(version, current_etag) != 0) {
        return PlotRead::Stale;
    }
    if (indexed_night->edf_catalog_pending) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return PlotRead::Building;
    }

    const uint64_t night_start_ms = indexed_night->summary.start_ms;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    if (range_plot_bytes_ && range_plot_index_ == resolved_therapy_index &&
        range_plot_night_start_ms_ == night_start_ms &&
        range_plot_from_ == from_ms && range_plot_to_ == to_ms) {
        const PlotBlobScan scan = scan_plot_blob(*range_plot_bytes_);
        if (!scan.valid) {
            range_plot_bytes_.reset();
            range_plot_index_ = 0;
            range_plot_night_start_ms_ = 0;
            range_plot_from_ = 0;
            range_plot_to_ = 0;
            xSemaphoreGive(result_slots_lock_);
            return PlotRead::Error;
        }
        if (scan.events == 0 && scan.points == 0) {
            xSemaphoreGive(result_slots_lock_);
            return PlotRead::Empty;
        }

        out = range_plot_bytes_;
        xSemaphoreGive(result_slots_lock_);
        return PlotRead::Ready;
    }

    if (range_plot_bytes_) {
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }

    range_req_active_ = true;
    range_req_index_ = resolved_therapy_index;
    range_req_night_start_ms_ = night_start_ms;
    range_req_from_ = from_ms;
    range_req_to_ = to_ms;
    xSemaphoreGive(result_slots_lock_);
    return PlotRead::Building;
}

}  // namespace aircannect
