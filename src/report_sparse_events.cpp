#include "report_manager.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {
namespace {

void format_stable_night_key(const ReportSummaryRecord &rec,
                             char *out,
                             size_t out_size) {
    if (!out || !out_size) return;
    snprintf(out, out_size, "%llu-%lu-%lu",
             static_cast<unsigned long long>(rec.start_ms),
             static_cast<unsigned long>(rec.duration_min),
             static_cast<unsigned long>(rec.session_interval_count));
}

}  // namespace

void ReportManager::clear_sparse_event_empty_markers(
    uint64_t night_start_ms) {
    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    const size_t marker_count =
        sizeof(sparse_event_empty_) / sizeof(sparse_event_empty_[0]);
    for (size_t i = 0; i < marker_count; ++i) {
        if (night_start_ms == 0 ||
            sparse_event_empty_[i].night_ms == night_start_ms) {
            sparse_event_empty_[i] = SparseEventEmptyMarker{};
        }
    }
    if (locked) xSemaphoreGive(prefetch_lock_);
}

void ReportManager::note_sparse_event_confirmed_empty(
    const ReportSummaryRecord &night,
    const ReportSourceDef &source) {
    char night_key[48];
    format_stable_night_key(night, night_key, sizeof(night_key));
    if (!night_key[0]) return;

    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    size_t pick = 0;
    bool found = false;
    const size_t marker_count =
        sizeof(sparse_event_empty_) / sizeof(sparse_event_empty_[0]);
    for (size_t i = 0; i < marker_count; ++i) {
        const SparseEventEmptyMarker &marker = sparse_event_empty_[i];
        if ((marker.source == source.id && marker.night_ms == night.start_ms) ||
            marker.night_ms == 0) {
            pick = i;
            found = true;
            break;
        }
    }
    if (!found) pick = 0;

    SparseEventEmptyMarker &marker = sparse_event_empty_[pick];
    marker.source = source.id;
    marker.night_ms = night.start_ms;
    snprintf(marker.night_key,
             sizeof(marker.night_key),
             "%s",
             night_key);
    if (locked) xSemaphoreGive(prefetch_lock_);
}

bool ReportManager::sparse_event_confirmed_empty(
    const ReportSummaryRecord &night,
    const ReportSourceDef &source) const {
    char night_key[48];
    format_stable_night_key(night, night_key, sizeof(night_key));
    if (!night_key[0]) return false;

    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    bool confirmed = false;
    const size_t marker_count =
        sizeof(sparse_event_empty_) / sizeof(sparse_event_empty_[0]);
    for (size_t i = 0; i < marker_count; ++i) {
        const SparseEventEmptyMarker &marker = sparse_event_empty_[i];
        if (marker.source == source.id &&
            marker.night_ms == night.start_ms &&
            marker.night_key[0] != '\0' &&
            strcmp(night_key, marker.night_key) == 0) {
            confirmed = true;
            break;
        }
    }
    if (locked) xSemaphoreGive(prefetch_lock_);
    return confirmed;
}

}  // namespace aircannect
