#include "report_result_slot_cache.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_plot_payload.h"

namespace aircannect {

struct ReportResultSlotCache::ResultCacheEntry {
    bool valid = false;
    uint64_t night_start_ms = 0;
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    uint32_t last_used = 0;
    ReportResultState state = ReportResultState::Idle;
    std::shared_ptr<ReportSpoolBuffer> result_json;
    std::shared_ptr<ReportSpoolBuffer> plot;
};

ReportResultSlotCache::~ReportResultSlotCache() {
    if (slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            slots_[i].~ResultCacheEntry();
        }
        Memory::free(slots_);
        slots_ = nullptr;
    }

    if (range_cache_) {
        range_cache_->~ReportRangePlotCache();
        Memory::free(range_cache_);
        range_cache_ = nullptr;
    }

    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

bool ReportResultSlotCache::ensure_lock() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    if (!lock_) {
        log_report_alloc_failed("result_slots_lock", 0);
        return false;
    }

    return true;
}

bool ReportResultSlotCache::ensure_slots() {
    if (!ensure_lock()) return false;
    if (!slots_) {
        slots_ = static_cast<ResultCacheEntry *>(Memory::alloc_large(
            AC_REPORT_RESULT_SLOT_MAX * sizeof(ResultCacheEntry), false));
        if (!slots_) {
            log_report_alloc_failed(
                "result_slots",
                AC_REPORT_RESULT_SLOT_MAX * sizeof(ResultCacheEntry));
            return false;
        }

        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            new (&slots_[i]) ResultCacheEntry();
        }
    }

    return ensure_range_cache();
}

bool ReportResultSlotCache::ensure_range_cache() {
    if (range_cache_) return true;

    range_cache_ = static_cast<ReportRangePlotCache *>(Memory::alloc_large(
        sizeof(ReportRangePlotCache), false));
    if (!range_cache_) {
        log_report_alloc_failed(
            "range_plot_cache",
            sizeof(ReportRangePlotCache));
        return false;
    }

    new (range_cache_) ReportRangePlotCache();

    return true;
}

void ReportResultSlotCache::apply_diagnostics(ReportResultStatus &status) const {
    if (!lock_) return;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return;

    status.materialized_slots = materialized_slots_;
    status.materialized_plot_slots = materialized_plot_slots_;

    xSemaphoreGive(lock_);
}

bool ReportResultSlotCache::publish(
    ReportResultState state,
    uint64_t night_start_ms,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &result_json,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!ensure_slots() || !etag || !etag[0] || night_start_ms == 0 ||
        ((!result_json || result_json->size() == 0) && !plot)) {
        return false;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (slots_[i].valid &&
            slots_[i].night_start_ms == night_start_ms) {
            pick = i;
            found = true;
            break;
        }
    }

    if (!found) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!slots_[i].valid) {
                pick = i;
                break;
            }
            if (slots_[i].last_used < slots_[pick].last_used) {
                pick = i;
            }
        }
    }

    ResultCacheEntry &slot = slots_[pick];
    if (slot.valid && strcmp(slot.etag, etag) != 0) {
        clear_slot_locked(slot);
    }

    slot.valid = true;
    slot.night_start_ms = night_start_ms;
    snprintf(slot.etag, sizeof(slot.etag), "%s", etag);
    if (result_json && result_json->size() > 0) {
        slot.state = state;
        slot.result_json = result_json;
    }
    if (plot) slot.plot = plot;
    slot.last_used = ++tick_;
    update_counts_locked();

    xSemaphoreGive(lock_);
    return true;
}

ReportResultSlotRead ReportResultSlotCache::read_result(
    uint64_t night_start_ms,
    const char *etag,
    LargeTextBuffer &json_out) {
    if (!slots_ || !lock_ || !etag || !etag[0]) {
        return ReportResultSlotRead::NotFound;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        ResultCacheEntry &slot = slots_[i];
        if (!slot.valid || slot.night_start_ms != night_start_ms) continue;

        if (strcmp(slot.etag, etag) != 0) {
            clear_slot_locked(slot);
            if (range_cache_) range_cache_->invalidate(night_start_ms, false);
            update_counts_locked();
            continue;
        }
        if (!slot.result_json) {
            xSemaphoreGive(lock_);
            return ReportResultSlotRead::NotFound;
        }

        slot.last_used = ++tick_;
        const std::shared_ptr<ReportSpoolBuffer> result_json =
            slot.result_json;

        xSemaphoreGive(lock_);

        json_out.clear();
        if (!json_out.append(
                reinterpret_cast<const char *>(result_json->data()),
                result_json->size())) {
            json_out.clear();
            return ReportResultSlotRead::Error;
        }

        return ReportResultSlotRead::Ready;
    }

    xSemaphoreGive(lock_);
    return ReportResultSlotRead::NotFound;
}

ReportCachedPlotRead ReportResultSlotCache::read_plot(
    uint64_t night_start_ms,
    const char *etag,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (!slots_ || !lock_ || !etag || !etag[0]) {
        return ReportCachedPlotRead::NotFound;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        ResultCacheEntry &slot = slots_[i];
        if (!slot.valid || slot.night_start_ms != night_start_ms) continue;

        if (strcmp(slot.etag, etag) != 0) {
            clear_slot_locked(slot);
            update_counts_locked();
            continue;
        }

        slot.last_used = ++tick_;
        if (slot.state == ReportResultState::Error) {
            xSemaphoreGive(lock_);
            return ReportCachedPlotRead::Error;
        }
        if (slot.plot) {
            out = slot.plot;
            xSemaphoreGive(lock_);
            return ReportCachedPlotRead::Ready;
        }

        xSemaphoreGive(lock_);
        return ReportCachedPlotRead::ResultWithoutPlot;
    }

    xSemaphoreGive(lock_);
    return ReportCachedPlotRead::NotFound;
}

ReportRangePlotRead ReportResultSlotCache::read_or_request_range(
    size_t index,
    uint64_t night_start_ms,
    const char *etag,
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (!etag || !etag[0] || !ensure_slots()) {
        return ReportRangePlotRead::Error;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    const ReportRangePlotRead result = range_cache_->read_or_request(
        index, night_start_ms, etag, from_ms, to_ms, out);
    xSemaphoreGive(lock_);
    return result;
}

bool ReportResultSlotCache::range_request_snapshot(
    ReportRangePlotRequest &out) const {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool active = range_cache_ && range_cache_->request_snapshot(out);
    xSemaphoreGive(lock_);
    return active;
}

void ReportResultSlotCache::finish_range_request(
    size_t index,
    uint64_t night_start_ms,
    const char *etag,
    int64_t from_ms,
    int64_t to_ms,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!lock_ || !range_cache_ || !etag || !etag[0] || !plot) return;

    const PlotBlobScan scan = scan_plot_blob(*plot);
    if (!scan.valid) {
        fail_range_request(index,
                           night_start_ms,
                           etag,
                           from_ms,
                           to_ms);
        return;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    range_cache_->finish_request(index,
                                 night_start_ms,
                                 etag,
                                 from_ms,
                                 to_ms,
                                 plot,
                                 scan.events == 0 && scan.points == 0);
    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::fail_range_request(size_t index,
                                               uint64_t night_start_ms,
                                               const char *etag,
                                               int64_t from_ms,
                                               int64_t to_ms) {
    if (!lock_ || !etag) return;

    xSemaphoreTake(lock_, portMAX_DELAY);
    if (range_cache_) {
        range_cache_->fail_request(index,
                                   night_start_ms,
                                   etag,
                                   from_ms,
                                   to_ms);
    }
    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::reset_range(bool clear_ready) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);
    if (range_cache_) range_cache_->reset(clear_ready);
    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::invalidate(uint64_t night_start_ms, bool all) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    if (slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (slots_[i].valid &&
                (all || slots_[i].night_start_ms == night_start_ms)) {
                clear_slot_locked(slots_[i]);
            }
        }
    }

    if (range_cache_) range_cache_->invalidate(night_start_ms, all);
    update_counts_locked();

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::clear_slot_locked(ResultCacheEntry &slot) {
    slot.valid = false;
    slot.night_start_ms = 0;
    slot.etag[0] = '\0';
    slot.last_used = 0;
    slot.state = ReportResultState::Idle;
    slot.result_json.reset();
    slot.plot.reset();
}

void ReportResultSlotCache::update_counts_locked() {
    uint32_t slots = 0;
    uint32_t plot_slots = 0;
    if (slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!slots_[i].valid) continue;
            slots++;
            if (slots_[i].plot && slots_[i].plot->size() > 0) {
                plot_slots++;
            }
        }
    }

    materialized_slots_ = slots;
    materialized_plot_slots_ = plot_slots;
}

}  // namespace aircannect
