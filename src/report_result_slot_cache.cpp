#include "report_result_slot_cache.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_manager_helpers.h"
#include "report_plot_payload.h"
#include "report_result_response_json.h"

namespace aircannect {

ReportResultSlotCache::~ReportResultSlotCache() {
    if (slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            slots_[i].~MaterializedResult();
        }
        Memory::free(slots_);
        slots_ = nullptr;
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
    if (slots_) return true;

    slots_ = static_cast<MaterializedResult *>(Memory::alloc_large(
        AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult), false));
    if (!slots_) {
        log_report_alloc_failed(
            "result_slots",
            AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        new (&slots_[i]) MaterializedResult();
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
    const ReportResultStatus &status,
    const ReportIndexedNight &night,
    const char *etag,
    const ReportSessionRange *ranges,
    size_t range_count,
    const ReportResultStream *streams,
    size_t stream_count,
    const ReportResultChunk *chunks,
    size_t chunk_count,
    const EdfReportSessionDescriptor *edf_sessions,
    size_t edf_session_count,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!ensure_slots() || !etag || !etag[0] || night.summary.start_ms == 0) {
        return false;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (slots_[i].valid &&
            slots_[i].night_start_ms == night.summary.start_ms) {
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

    MaterializedResult &slot = slots_[pick];
    slot.valid = true;
    slot.night_start_ms = night.summary.start_ms;
    snprintf(slot.etag, sizeof(slot.etag), "%s", etag);
    slot.status = status;
    slot.night = night;

    slot.range_count =
        std::min(range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_NIGHT_SESSION_MAX; ++i) {
        slot.ranges[i] = (ranges && i < slot.range_count)
                             ? ranges[i]
                             : ReportSessionRange{};
    }

    slot.stream_count =
        std::min(stream_count,
                 static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        slot.streams[i] = (streams && i < slot.stream_count)
                              ? streams[i]
                              : ReportResultStream{};
    }

    slot.chunk_count =
        std::min(chunk_count, static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_CHUNK_MAX; ++i) {
        slot.chunks[i] = (chunks && i < slot.chunk_count) ? chunks[i]
                                                          : ReportResultChunk{};
    }

    slot.edf_session_count =
        std::min(edf_session_count,
                 static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_EDF_SESSION_MAX; ++i) {
        slot.edf_sessions[i] =
            (edf_sessions && i < slot.edf_session_count)
                ? edf_sessions[i]
                : EdfReportSessionDescriptor{};
    }

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
        MaterializedResult &slot = slots_[i];
        if (!slot.valid || slot.night_start_ms != night_start_ms) continue;

        if (strcmp(slot.etag, etag) != 0 ||
            !report_manager_internal::result_state_materialized_slot_allowed(
                slot.status.state)) {
            clear_slot_locked(slot);
            clear_range_locked(night_start_ms, false);
            update_counts_locked();
            continue;
        }

        slot.last_used = ++tick_;
        const bool cacheable = slot.status.state == ReportResultState::Ready ||
                               slot.status.state == ReportResultState::Partial;
        if (cacheable && if_none_match && if_none_match[0] &&
            strcmp(if_none_match, etag) == 0) {
            xSemaphoreGive(lock_);
            return ReportResultSlotRead::NotModified;
        }

        const ReportCacheFetchStatus inactive{};
        build_report_result_json_from(
            slot.status,
            slot.night,
            slot.ranges,
            std::min(slot.range_count,
                     static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX)),
            slot.streams,
            std::min(slot.stream_count,
                     static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX)),
            inactive,
            json_out);

        xSemaphoreGive(lock_);
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
        MaterializedResult &slot = slots_[i];
        if (!slot.valid || slot.night_start_ms != night_start_ms) continue;

        if (strcmp(slot.etag, etag) != 0) {
            clear_slot_locked(slot);
            update_counts_locked();
            continue;
        }

        slot.last_used = ++tick_;
        if (slot.status.state == ReportResultState::Error) {
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
        MaterializedResult &slot = slots_[i];
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
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (!ensure_lock()) return ReportRangePlotRead::Error;

    xSemaphoreTake(lock_, portMAX_DELAY);

    if (range_plot_bytes_ && range_plot_index_ == index &&
        range_plot_night_start_ms_ == night_start_ms &&
        range_plot_from_ == from_ms && range_plot_to_ == to_ms) {
        const PlotBlobScan scan = scan_plot_blob(*range_plot_bytes_);
        if (!scan.valid) {
            clear_range_locked(night_start_ms, true);
            xSemaphoreGive(lock_);
            return ReportRangePlotRead::Error;
        }
        if (scan.events == 0 && scan.points == 0) {
            xSemaphoreGive(lock_);
            return ReportRangePlotRead::Empty;
        }

        out = range_plot_bytes_;
        xSemaphoreGive(lock_);
        return ReportRangePlotRead::Ready;
    }

    if (range_plot_bytes_) {
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }

    range_req_active_ = true;
    range_req_index_ = index;
    range_req_night_start_ms_ = night_start_ms;
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
    out.from_ms = range_req_from_;
    out.to_ms = range_req_to_;

    xSemaphoreGive(lock_);
    return out.active;
}

void ReportResultSlotCache::finish_range_request(
    size_t index,
    uint64_t night_start_ms,
    int64_t from_ms,
    int64_t to_ms,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!lock_ || !plot) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    if (range_req_active_ && range_req_index_ == index &&
        range_req_night_start_ms_ == night_start_ms &&
        range_req_from_ == from_ms && range_req_to_ == to_ms) {
        range_plot_bytes_ = plot;
        range_plot_index_ = index;
        range_plot_night_start_ms_ = night_start_ms;
        range_plot_from_ = from_ms;
        range_plot_to_ = to_ms;
        range_req_active_ = false;
    }

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::fail_range_request(size_t index,
                                               uint64_t night_start_ms,
                                               int64_t from_ms,
                                               int64_t to_ms) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    if (range_req_active_ && range_req_index_ == index &&
        range_req_night_start_ms_ == night_start_ms &&
        range_req_from_ == from_ms && range_req_to_ == to_ms) {
        range_req_active_ = false;
    }

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::reset_range(bool clear_ready) {
    if (!lock_) {
        range_req_active_ = false;
        range_req_night_start_ms_ = 0;
        if (clear_ready) range_plot_bytes_.reset();
        return;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);

    range_req_active_ = false;
    range_req_night_start_ms_ = 0;
    if (clear_ready) {
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::invalidate(uint64_t night_start_ms, bool all) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    bool invalidated = false;
    if (slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (slots_[i].valid &&
                (all || slots_[i].night_start_ms == night_start_ms)) {
                clear_slot_locked(slots_[i]);
                invalidated = true;
            }
        }
    }

    if (invalidated) clear_range_locked(night_start_ms, all);
    update_counts_locked();

    xSemaphoreGive(lock_);
}

void ReportResultSlotCache::clear_slot_locked(MaterializedResult &slot) {
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

}  // namespace aircannect
