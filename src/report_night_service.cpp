#include "report_manager.h"

#include <algorithm>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "edf_report_provider.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_diagnostics.h"
#include "report_index_scratch.h"
#include "report_resolve_context.h"
#include "report_source_resolver.h"
#include "report_store.h"
#include "report_summary_json.h"

namespace aircannect {
namespace {

constexpr uint32_t REPORT_RESULT_ETAG_VERSION = 29;

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

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

// Map a timestamp to its summary-night bucket start (the partition key). Bucket
// boundaries sit around local noon (no therapy), so a chunk straddling one is
// filed whole by its start timestamp.
int64_t ReportManager::night_start_for_timestamp(int64_t timestamp_ms) const {
    if (!take_summary_lock(portMAX_DELAY)) return timestamp_ms;
    if (!records_ || record_count_ == 0) {
        give_summary_lock();
        return timestamp_ms;
    }
    int64_t nearest_start = 0;
    bool have_nearest = false;
    for (size_t i = 0; i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &r = records_[i];
        if (!r.valid || !r.duration_min) continue;
        const int64_t s = static_cast<int64_t>(r.start_ms);
        const int64_t e = static_cast<int64_t>(r.end_ms);
        if (timestamp_ms >= s && timestamp_ms < e) {
            give_summary_lock();
            return s;
        }
        if (s <= timestamp_ms && (!have_nearest || s > nearest_start)) {
            nearest_start = s;
            have_nearest = true;
        }
    }
    const int64_t result = have_nearest ? nearest_start : timestamp_ms;
    give_summary_lock();
    return result;
}

uint32_t ReportManager::night_epoch_for_unlocked(uint64_t night_start_ms) const {
    if (!night_epochs_) return 0;
    for (size_t i = 0; i < night_epoch_count_; ++i) {
        if (night_epochs_[i].night_start_ms == night_start_ms) {
            return night_epochs_[i].epoch;
        }
    }
    return 0;
}

void ReportManager::format_night_etag_unlocked(
    const ReportSummaryRecord &rec,
    uint64_t source_signature,
    char *out,
    size_t out_size) const {
    if (!out || !out_size) return;
    snprintf(out, out_size, "%llu-%lu-%lu-%08lx%08lx-%lu-r%lu",
             static_cast<unsigned long long>(rec.start_ms),
             static_cast<unsigned long>(rec.duration_min),
             static_cast<unsigned long>(rec.session_interval_count),
             static_cast<unsigned long>(source_signature >> 32),
             static_cast<unsigned long>(source_signature & 0xffffffffULL),
             static_cast<unsigned long>(night_epoch_for_unlocked(rec.start_ms)),
             static_cast<unsigned long>(REPORT_RESULT_ETAG_VERSION));
}

bool ReportManager::night_etag(size_t therapy_index, char *out,
                               size_t out_size) const {
    ScopedIndexedNight night("night_etag_index");
    if (!night ||
        !indexed_night_by_therapy_index(therapy_index, night.get())) {
        return false;
    }
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    format_night_etag_unlocked(night->summary,
                               night->source_signature,
                               out,
                               out_size);
    give_summary_lock();
    return true;
}

bool ReportManager::night_coverage(uint64_t night_start_ms,
                                   ReportNightCoverageStatus &out) const {
    out = {};
    ScopedIndexedNight indexed_night("night_coverage_index");
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms, indexed_night.get())) {
        return false;
    }
    const ReportIndexedNight &indexed = indexed_night.get();
    const ReportSummaryRecord &night = indexed.summary;

    out.found = true;
    out.start_ms = night.start_ms;
    out.end_ms = night.end_ms;
    out.duration_min = report_indexed_night_display_duration_min(indexed);

    int64_t span_start_ms = 0;
    int64_t span_end_ms = 0;
    if (!indexed_night_data_span(indexed, span_start_ms, span_end_ms)) {
        return true;
    }

    ScopedReportResolveContext resolve("night_coverage_resolver");
    if (!resolve) {
        return false;
    }

    bool pending = false;
    size_t session_count = 0;
    if (!collect_edf_sessions_for_night(night,
                                        span_start_ms,
                                        span_end_ms,
                                        resolve.sessions(),
                                        AC_REPORT_EDF_SESSION_MAX,
                                        session_count,
                                        &pending) ||
        pending) {
        return false;
    }

    EdfReportDataProvider edf_provider(resolve.sessions(), session_count);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  resolve.scratch());
    if (!resolver.build_plan(indexed,
                             span_start_ms,
                             span_end_ms,
                             resolve.plan())) {
        return false;
    }

    const ReportResolvedPlan &plan = resolve.plan();
    out.missing_required = plan.missing_required;
    for (size_t i = 0; i < plan.stream_count; ++i) {
        const ReportResolvedStream &stream = plan.streams[i];
        if (!cache_source_supported(stream.selected_source)) continue;
        ReportNightSourceCoverage *entry = nullptr;
        for (size_t existing = 0; existing < out.source_count; ++existing) {
            if (out.sources[existing].source == stream.selected_source) {
                entry = &out.sources[existing];
                break;
            }
        }
        if (!entry) {
            if (out.source_count >= AC_REPORT_NIGHT_SOURCE_MAX) break;
            entry = &out.sources[out.source_count++];
            entry->source = stream.selected_source;
            entry->complete = true;
        }
        entry->required = entry->required || stream.required;
        if (stream.required && !stream.complete) entry->complete = false;
    }
    return true;
}

bool ReportManager::next_night_needing_cache(
    uint64_t &night_start_ms_out) const {
    const uint32_t now = millis();
    ReportIndexedNight *nights =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!nights) {
        log_report_alloc_failed(
            "prefetch_night_index",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }
    size_t count = 0;
    if (!build_indexed_nights(nights,
                              AC_REPORT_SUMMARY_RECORD_MAX,
                              count)) {
        Memory::free(nights);
        return false;
    }
    ScopedReportResolveContext resolve("prefetch_resolver");
    if (!resolve) {
        Memory::free(nights);
        return false;
    }

    // Oldest-first: the spool is open-ended (fromDateTime -> now), so fetching
    // the OLDEST night with a gap streams every source from there forward and
    // backfills all newer nights in a single sweep (deduped on write)
    for (size_t i = 0; i < count; ++i) {
        const ReportIndexedNight &indexed = nights[i];
        const ReportSummaryRecord &record = indexed.summary;
        if (!record.valid || !record.duration_min) continue;
        if (prefetch_in_cooldown(record.start_ms, now)) continue;
        int64_t span_start_ms = 0;
        int64_t span_end_ms = 0;
        if (!indexed_night_data_span(indexed, span_start_ms, span_end_ms)) {
            continue;
        }

        bool edf_pending = false;
        size_t session_count = 0;
        memset(resolve.sessions(),
               0,
               AC_REPORT_EDF_SESSION_MAX *
                   sizeof(EdfReportSessionDescriptor));
        if (!collect_edf_sessions_for_night(record,
                                            span_start_ms,
                                            span_end_ms,
                                            resolve.sessions(),
                                            AC_REPORT_EDF_SESSION_MAX,
                                            session_count,
                                            &edf_pending)) {
            Memory::free(nights);
            return false;
        }
        if (edf_pending) continue;

        EdfReportDataProvider edf_provider(resolve.sessions(), session_count);
        ReportSourceResolver resolver(edf_provider,
                                      spool_report_provider(),
                                      resolve.scratch());
        if (!resolver.build_plan(indexed,
                                 span_start_ms,
                                 span_end_ms,
                                 resolve.plan())) {
            Memory::free(nights);
            return false;
        }
        const ReportResolvedPlan &plan = resolve.plan();
        for (size_t segment_index = 0; segment_index < plan.segment_count;
             ++segment_index) {
            const ReportResolvedSegment &segment =
                plan.segments[segment_index];
            if (segment.provider == ReportResolvedProvider::Spool &&
                !segment.complete &&
                segment.required &&
                cache_source_supported(segment.source)) {
                night_start_ms_out = record.start_ms;
                Memory::free(nights);
                return true;
            }
        }
    }
    Memory::free(nights);
    return false;
}

bool ReportManager::for_each_summary_night(
    ReportSummaryNightCallback callback,
    void *context) const {
    if (!callback) return false;

    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "summary_night_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t count = 0;
    if (!build_indexed_nights(snapshot,
                              AC_REPORT_SUMMARY_RECORD_MAX,
                              count)) {
        Memory::free(snapshot);
        return false;
    }

    bool any = false;
    size_t therapy_index = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t summary_index = i - 1;
        const ReportIndexedNight &indexed = snapshot[summary_index];
        if (!report_indexed_night_visible_in_summary(indexed)) continue;
        ReportSummaryRecord record = indexed.summary;
        record.duration_min = report_indexed_night_display_duration_min(indexed);

        ReportSummaryNight night;
        night.summary_index = summary_index;
        night.therapy_index = therapy_index++;
        night.record = record;
        any = true;
        if (!callback(context, night)) break;
    }
    Memory::free(snapshot);
    return any;
}

bool ReportManager::summary_night_by_therapy_index(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    ScopedIndexedNight night("summary_night_index");
    if (!night ||
        !indexed_night_by_therapy_index(therapy_index, night.get())) {
        return false;
    }
    out = night->summary;
    out.duration_min = report_indexed_night_display_duration_min(night.get());
    return true;
}

bool ReportManager::latest_summary_night(ReportSummaryRecord &out) const {
    return summary_night_by_therapy_index(0, out);
}

bool ReportManager::night_coverage_by_therapy_index(
    size_t therapy_index,
    ReportNightCoverageStatus &out) const {
    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) return false;
    return night_coverage(night.start_ms, out);
}

bool ReportManager::latest_night_coverage(
    ReportNightCoverageStatus &out) const {
    return night_coverage_by_therapy_index(0, out);
}

bool ReportManager::request_night_cache(uint64_t night_start_ms, bool force) {
    if (summary_fetch_active_ || cache_fetch_active_ || range_build_active_) {
        return false;
    }
    ScopedIndexedNight indexed_night("request_night_cache_index");
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms, indexed_night.get()) ||
        !report_indexed_night_visible_in_summary(indexed_night.get())) {
        return false;
    }
    if (!build_cache_plan(indexed_night.get(), force, false)) return false;
    if (cache_source_count_ == 0) {
        finish_cache_fetch();
        return true;
    }
    return start_next_cache_source();
}

bool ReportManager::request_night_cache_by_therapy_index(
    size_t therapy_index,
    bool force) {
    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) return false;
    return request_night_cache(night.start_ms, force);
}

bool ReportManager::request_latest_night_cache(bool force) {
    return request_night_cache_by_therapy_index(0, force);
}

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
