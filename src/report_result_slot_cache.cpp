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

struct ReportResultSlotCache::RangePlotCacheEntry {
    bool valid = false;
    bool empty = false;
    uint64_t night_start_ms = 0;
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    int64_t from_ms = 0;
    int64_t to_ms = 0;
    uint32_t last_used = 0;
    std::shared_ptr<ReportSpoolBuffer> bytes;
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
        for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            range_cache_[i].~RangePlotCacheEntry();
        }
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

    range_cache_ = static_cast<RangePlotCacheEntry *>(Memory::alloc_large(
        AC_REPORT_RANGE_CACHE_SLOT_MAX * sizeof(RangePlotCacheEntry), false));
    if (!range_cache_) {
        log_report_alloc_failed(
            "range_plot_cache",
            AC_REPORT_RANGE_CACHE_SLOT_MAX * sizeof(RangePlotCacheEntry));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
        new (&range_cache_[i]) RangePlotCacheEntry();
    }

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
        !result_json || result_json->size() == 0) {
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
    slot.valid = true;
    slot.night_start_ms = night_start_ms;
    snprintf(slot.etag, sizeof(slot.etag), "%s", etag);
    slot.state = state;
    slot.result_json = result_json;
    slot.plot = plot;
    slot.last_used = ++tick_;
    update_counts_locked();

    xSemaphoreGive(lock_);
    return true;
}

ReportResultSlotRead ReportResultSlotCache::read_result(
    uint64_t night_start_ms,
    const char *etag,
    const char *if_none_match,
    LargeTextBuffer &json_out) {
    if (!slots_ || !lock_ || !etag || !etag[0]) {
        return ReportResultSlotRead::NotFound;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        ResultCacheEntry &slot = slots_[i];
        if (!slot.valid || slot.night_start_ms != night_start_ms) continue;

        if (strcmp(slot.etag, etag) != 0 || !slot.result_json) {
            clear_slot_locked(slot);
            clear_range_locked(night_start_ms, false);
            update_counts_locked();
            continue;
        }

        slot.last_used = ++tick_;
        const bool cacheable = slot.state == ReportResultState::Ready ||
                               slot.state == ReportResultState::Partial;
        if (cacheable && if_none_match && if_none_match[0] &&
            strcmp(if_none_match, etag) == 0) {
            xSemaphoreGive(lock_);
            return ReportResultSlotRead::NotModified;
        }

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

bool ReportResultSlotCache::attach_plot(
    uint64_t night_start_ms,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!slots_ || !lock_ || !etag || !etag[0] || !plot) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);

    bool attached = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        ResultCacheEntry &slot = slots_[i];
        if (!slot.valid || slot.night_start_ms != night_start_ms) continue;

        if (strcmp(slot.etag, etag) != 0) {
            clear_slot_locked(slot);
            update_counts_locked();
            continue;
        }

        slot.plot = plot;
        slot.last_used = ++tick_;
        update_counts_locked();
        attached = true;
        break;
    }

    xSemaphoreGive(lock_);
    return attached;
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

    for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
        RangePlotCacheEntry &entry = range_cache_[i];
        if (!entry.valid || entry.night_start_ms != night_start_ms ||
            strcmp(entry.etag, etag) != 0 || entry.from_ms != from_ms ||
            entry.to_ms != to_ms) {
            continue;
        }
        if (!entry.bytes) {
            clear_range_entry_locked(entry);
            continue;
        }

        entry.last_used = ++range_cache_tick_;
        out = entry.bytes;
        const ReportRangePlotRead result =
            entry.empty ? ReportRangePlotRead::Empty
                        : ReportRangePlotRead::Ready;
        xSemaphoreGive(lock_);
        return result;
    }

    if (range_req_active_ &&
        range_req_night_start_ms_ == night_start_ms &&
        strcmp(range_req_etag_, etag) == 0 &&
        range_req_from_ == from_ms && range_req_to_ == to_ms) {
        xSemaphoreGive(lock_);
        return ReportRangePlotRead::Building;
    }

    range_req_active_ = true;
    range_req_index_ = index;
    range_req_night_start_ms_ = night_start_ms;
    snprintf(range_req_etag_, sizeof(range_req_etag_), "%s", etag);
    range_req_from_ = from_ms;
    range_req_to_ = to_ms;

    xSemaphoreGive(lock_);
    return ReportRangePlotRead::Building;
}

bool ReportResultSlotCache::range_request_snapshot(
    ReportRangePlotRequest &out) const {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);

    out.active = range_req_active_;
    out.index = range_req_index_;
    out.night_start_ms = range_req_night_start_ms_;
    snprintf(out.etag, sizeof(out.etag), "%s", range_req_etag_);
    out.from_ms = range_req_from_;
    out.to_ms = range_req_to_;

    xSemaphoreGive(lock_);
    return out.active;
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

    if (range_req_active_ && range_req_index_ == index &&
        range_req_night_start_ms_ == night_start_ms &&
        strcmp(range_req_etag_, etag) == 0 &&
        range_req_from_ == from_ms && range_req_to_ == to_ms) {
        size_t pick = 0;
        bool have_pick = false;

        for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            RangePlotCacheEntry &entry = range_cache_[i];
            if (entry.valid && entry.night_start_ms == night_start_ms &&
                strcmp(entry.etag, etag) == 0 && entry.from_ms == from_ms &&
                entry.to_ms == to_ms) {
                pick = i;
                have_pick = true;
                break;
            }
        }

        if (!have_pick) {
            for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
                if (range_cache_[i].valid) continue;

                pick = i;
                have_pick = true;
                break;
            }
        }

        if (!have_pick) {
            for (size_t i = 1; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
                if (range_cache_[i].last_used < range_cache_[pick].last_used) {
                    pick = i;
                }
            }
        }

        clear_range_entry_locked(range_cache_[pick]);
        trim_range_cache_locked(plot->size(), pick);

        RangePlotCacheEntry &entry = range_cache_[pick];
        entry.valid = true;
        entry.empty = scan.events == 0 && scan.points == 0;
        entry.night_start_ms = night_start_ms;
        snprintf(entry.etag, sizeof(entry.etag), "%s", etag);
        entry.from_ms = from_ms;
        entry.to_ms = to_ms;
        entry.last_used = ++range_cache_tick_;
        entry.bytes = plot;

        range_req_active_ = false;
        range_req_index_ = 0;
        range_req_night_start_ms_ = 0;
        range_req_etag_[0] = '\0';
        range_req_from_ = 0;
        range_req_to_ = 0;
    }

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::fail_range_request(size_t index,
                                               uint64_t night_start_ms,
                                               const char *etag,
                                               int64_t from_ms,
                                               int64_t to_ms) {
    if (!lock_ || !etag) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    if (range_req_active_ && range_req_index_ == index &&
        range_req_night_start_ms_ == night_start_ms &&
        strcmp(range_req_etag_, etag) == 0 &&
        range_req_from_ == from_ms && range_req_to_ == to_ms) {
        range_req_active_ = false;
        range_req_index_ = 0;
        range_req_night_start_ms_ = 0;
        range_req_etag_[0] = '\0';
        range_req_from_ = 0;
        range_req_to_ = 0;
    }

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::reset_range(bool clear_ready) {
    if (!lock_) {
        range_req_active_ = false;
        range_req_night_start_ms_ = 0;
        range_req_etag_[0] = '\0';
        return;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    range_req_active_ = false;
    range_req_index_ = 0;
    range_req_night_start_ms_ = 0;
    range_req_etag_[0] = '\0';
    range_req_from_ = 0;
    range_req_to_ = 0;
    if (clear_ready && range_cache_) {
        for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            clear_range_entry_locked(range_cache_[i]);
        }
    }

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

    clear_range_locked(night_start_ms, all);
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

void ReportResultSlotCache::clear_range_entry_locked(
    RangePlotCacheEntry &entry) {
    entry.valid = false;
    entry.empty = false;
    entry.night_start_ms = 0;
    entry.etag[0] = '\0';
    entry.from_ms = 0;
    entry.to_ms = 0;
    entry.last_used = 0;
    entry.bytes.reset();
}

void ReportResultSlotCache::trim_range_cache_locked(
    size_t incoming_bytes,
    size_t protected_index) {
    if (!range_cache_) return;

    size_t retained_bytes = 0;
    for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
        const RangePlotCacheEntry &entry = range_cache_[i];
        if (entry.valid && entry.bytes) retained_bytes += entry.bytes->size();
    }

    while (retained_bytes > AC_REPORT_RANGE_CACHE_MAX_BYTES ||
           incoming_bytes > AC_REPORT_RANGE_CACHE_MAX_BYTES -
                                retained_bytes) {
        size_t victim = AC_REPORT_RANGE_CACHE_SLOT_MAX;
        for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            const RangePlotCacheEntry &entry = range_cache_[i];
            if (i == protected_index || !entry.valid) continue;
            if (victim == AC_REPORT_RANGE_CACHE_SLOT_MAX ||
                entry.last_used < range_cache_[victim].last_used) {
                victim = i;
            }
        }
        if (victim == AC_REPORT_RANGE_CACHE_SLOT_MAX) break;

        if (range_cache_[victim].bytes) {
            retained_bytes -= range_cache_[victim].bytes->size();
        }
        clear_range_entry_locked(range_cache_[victim]);
    }
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

void ReportResultSlotCache::clear_range_locked(uint64_t night_start_ms,
                                               bool all) {
    const bool matches_request =
        range_req_active_ &&
        (all || range_req_night_start_ms_ == night_start_ms);

    if (matches_request) {
        range_req_active_ = false;
        range_req_index_ = 0;
        range_req_night_start_ms_ = 0;
        range_req_etag_[0] = '\0';
        range_req_from_ = 0;
        range_req_to_ = 0;
    }

    if (!range_cache_) return;
    for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
        RangePlotCacheEntry &entry = range_cache_[i];
        if (entry.valid && (all || entry.night_start_ms == night_start_ms)) {
            clear_range_entry_locked(entry);
        }
    }
}

}  // namespace aircannect
