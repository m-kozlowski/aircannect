#include "report_manager.h"

#include "debug_log.h"
#include "memory_manager.h"
#include "report_cache_paths.h"
#include "report_diagnostics.h"
#include "report_sources.h"
#include "report_store.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

void merge_cache_clear_result(ReportCacheClearResult &dst,
                              const ReportCacheClearResult &src) {
    dst.store_reset += src.store_reset;
    dst.summary_deleted += src.summary_deleted;
    dst.nights_cleared += src.nights_cleared;
    dst.chunks_deleted += src.chunks_deleted;
    dst.coverage_deleted += src.coverage_deleted;
    dst.plots_deleted += src.plots_deleted;
    dst.result_json_deleted += src.result_json_deleted;
}

}  // namespace

bool ReportManager::clear_plot_cache_for_night(
    const ReportSummaryRecord &night,
    uint32_t &deleted) const {
    deleted = 0;
    if (!night.start_ms) return false;

    Storage::Guard g;
    if (!Storage::mounted()) return false;

    File dir = Storage::open(REPORT_PLOT_CACHE_DIR, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;

        const bool is_dir = file.isDirectory();
        const bool match =
            !is_dir && plot_cache_name_for_night(file.name(), night.start_ms);

        char path[REPORT_CACHE_PATH_MAX];
        const bool path_ok = match && cache_child_path(REPORT_PLOT_CACHE_DIR,
                                                       file.name(),
                                                       path,
                                                       sizeof(path));
        file.close();

        if (!match) continue;
        if (!path_ok || !Storage::remove(path)) {
            ok = false;
            continue;
        }

        deleted++;
    }
    dir.close();

    return ok;
}

bool ReportManager::clear_cache_range(int64_t start_ms,
                                      int64_t end_ms,
                                      ReportCacheClearResult &out) {
    if (start_ms < 0 || end_ms <= start_ms) return false;

    ReportSummaryRecord *night_batch = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, night_batch)) return false;

    size_t night_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }

    for (size_t n = 0; records_ && n < record_count_ &&
                       n < AC_REPORT_SUMMARY_RECORD_MAX; ++n) {
        const ReportSummaryRecord &r = records_[n];
        if (!r.valid) continue;

        const int64_t ns = static_cast<int64_t>(r.start_ms);
        const int64_t ne = static_cast<int64_t>(r.end_ms);
        if (ne <= ns || ns >= end_ms || ne <= start_ms) continue;
        if (night_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;

        night_batch[night_count++] = r;
    }
    give_summary_lock();

    bool ok = true;
    uint32_t deleted = 0;

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);
    for (size_t i = 0; i < source_count; ++i) {
        const ReportSourceDef &source = sources[i];
        if (source.id == ReportSourceId::Summary) continue;
        if (!source.spool_type || !source.spool_type[0]) continue;

        deleted = 0;
        if (!ReportStore::clear_coverage(source.spool_type,
                                         start_ms,
                                         end_ms,
                                         deleted)) {
            ok = false;
        }
        out.coverage_deleted += deleted;

        // Chunks live in per-night dirs; clear each night overlapping the range.
        for (size_t n = 0; n < night_count; ++n) {
            const ReportSummaryRecord &r = night_batch[n];
            const int64_t ns = static_cast<int64_t>(r.start_ms);

            deleted = 0;
            if (!ReportStore::clear_chunks(ReportStoreChunkKind::Events,
                                           source.spool_type,
                                           source.spool_type,
                                           ns,
                                           start_ms,
                                           end_ms,
                                           deleted)) {
                ok = false;
            }
            out.chunks_deleted += deleted;
        }
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        const ReportSignalDef &signal = signals[i];
        const ReportSourceId source_ids[] = {
            signal.preferred_source,
            signal.fallback_source,
        };

        for (ReportSourceId source_id : source_ids) {
            const ReportSourceDef *source = report_source_def(source_id);
            if (!source || source->id == ReportSourceId::Summary ||
                !source->spool_type || !source->spool_type[0] ||
                !signal.store_name || !signal.store_name[0]) {
                continue;
            }

            for (size_t n = 0; n < night_count; ++n) {
                const ReportSummaryRecord &r = night_batch[n];
                const int64_t ns = static_cast<int64_t>(r.start_ms);

                deleted = 0;
                if (!ReportStore::clear_chunks(ReportStoreChunkKind::Series,
                                               source->spool_type,
                                               signal.store_name,
                                               ns,
                                               start_ms,
                                               end_ms,
                                               deleted)) {
                    ok = false;
                }
                out.chunks_deleted += deleted;
            }
        }
    }

    give_summary_scratch();
    return ok;
}

bool ReportManager::clear_cache_all(ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    uint32_t store_reset = 0;
    if (!ReportStore::reset_cache_store(store_reset)) {
        return false;
    }
    out.store_reset = store_reset;

    clear_build_queue(0, true);
    invalidate_materialized(0, true);
    clear_sparse_event_empty_markers(0);

    if (!take_summary_lock(portMAX_DELAY)) return false;
    clear_summary_records();
    night_epoch_count_ = 0;
    summary_status_.state = ReportSummaryState::Idle;
    summary_status_.revision++;
    summary_status_.records_total = 0;
    summary_status_.nights_with_therapy = 0;
    summary_status_.elapsed_ms = 0;
    summary_status_.active_spool.clear();
    summary_status_.error.clear();
    give_summary_lock();

    publish_summary_json_snapshot();
    clear_result_prepare();

    return true;
}

bool ReportManager::clear_cache_night(uint64_t night_start_ms,
                                      ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    ReportIndexedNight *indexed_night =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            sizeof(ReportIndexedNight),
            false));
    if (!indexed_night) {
        log_report_alloc_failed("clear_cache_night_index",
                                sizeof(ReportIndexedNight));
        return false;
    }

    const bool found =
        indexed_night_by_start(night_start_ms, *indexed_night) &&
        indexed_night->summary.end_ms > indexed_night->summary.start_ms;
    const ReportSummaryRecord night = found ? indexed_night->summary
                                            : ReportSummaryRecord{};
    Memory::free(indexed_night);

    if (!found) return false;

    clear_build_queue(night.start_ms, false);
    invalidate_materialized(night.start_ms, false);

    const bool ok = clear_cache_range(static_cast<int64_t>(night.start_ms),
                                      static_cast<int64_t>(night.end_ms),
                                      out);

    clear_sparse_event_empty_markers(night.start_ms);

    uint32_t plot_deleted = 0;
    if (clear_plot_cache_for_night(night, plot_deleted)) {
        out.plots_deleted += plot_deleted;
    }

    uint32_t result_json_deleted = 0;
    if (clear_result_json_cache_for_night(night, result_json_deleted)) {
        out.result_json_deleted += result_json_deleted;
    }

    if (result_status_.night_start_ms == night.start_ms) {
        clear_result_prepare();
    }

    if (take_summary_lock(portMAX_DELAY)) {
        for (size_t i = 0; i < night_epoch_count_; ++i) {
            if (night_epochs_[i].night_start_ms == night.start_ms) {
                night_epochs_[i] = night_epochs_[night_epoch_count_ - 1];
                --night_epoch_count_;
                break;
            }
        }
        give_summary_lock();
    }

    if (ok) out.nights_cleared = 1;
    return ok;
}

bool ReportManager::clear_oldest_cache_nights(size_t max_nights,
                                              ReportCacheClearResult &out) {
    out = {};
    if (max_nights == 0) return true;
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    ReportSummaryRecord *snapshot =
        static_cast<ReportSummaryRecord *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "cache_prune_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        return false;
    }

    size_t count = 0;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        Memory::free(snapshot);
        return false;
    }

    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &record = records_[i];
        if (!record.valid || record.end_ms <= record.start_ms) continue;

        snapshot[count++] = record;
    }
    give_summary_lock();

    bool ok = true;
    const size_t limit = count < max_nights ? count : max_nights;
    for (size_t i = 0; i < limit; ++i) {
        ReportCacheClearResult current_clear;
        if (!clear_cache_night(snapshot[i].start_ms, current_clear)) {
            ok = false;
            break;
        }

        merge_cache_clear_result(out, current_clear);
    }
    Memory::free(snapshot);

    if (out.nights_cleared > 0) {
        Log::logf(CAT_REPORT,
                  LOG_INFO,
                  "Cache pruned oldest nights=%lu chunks=%lu coverage=%lu "
                  "plots=%lu result_json=%lu\n",
                  static_cast<unsigned long>(out.nights_cleared),
                  static_cast<unsigned long>(out.chunks_deleted),
                  static_cast<unsigned long>(out.coverage_deleted),
                  static_cast<unsigned long>(out.plots_deleted),
                  static_cast<unsigned long>(out.result_json_deleted));
    }

    return ok;
}

bool ReportManager::prune_cache_to_latest_nights(size_t keep_latest,
                                                 ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    size_t report_nights = 0;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;

    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &record = records_[i];
        if (!record.valid || record.end_ms <= record.start_ms) continue;

        report_nights++;
    }
    give_summary_lock();

    if (report_nights <= keep_latest) return true;
    return clear_oldest_cache_nights(report_nights - keep_latest, out);
}

}  // namespace aircannect
