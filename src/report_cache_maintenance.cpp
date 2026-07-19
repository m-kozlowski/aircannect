#include "report_manager.h"

#include "debug_log.h"
#include "memory_manager.h"
#include "report_build_queue_service.h"
#include "report_cache_fetch_service.h"
#include "report_cache_maintenance_service.h"
#include "report_cache_paths.h"
#include "report_diagnostics.h"
#include "report_night_index_runtime.h"
#include "report_night_index_service.h"
#include "report_range_plot_builder.h"
#include "report_result_build_service.h"
#include "report_result_cache_runtime.h"
#include "report_result_prepare_service.h"
#include "report_sources.h"
#include "report_store.h"
#include "report_summary_runtime.h"
#include "report_summary_service.h"
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

ReportCacheMaintenanceService::ReportCacheMaintenanceService(
    ReportSummaryRuntime &summary_runtime,
    ReportNightIndexRuntime &night_index_runtime,
    ReportNightIndexService &night_index,
    ReportSummaryService &summary,
    ReportCacheFetchService &cache_fetch,
    ReportResultCacheRuntime &result_cache,
    ReportResultBuildService &result_build,
    ReportRangePlotBuilder &range_plot,
    ReportResultPrepareService &result_prepare,
    ReportBuildQueueService &build_queue)
    : summary_runtime_(summary_runtime),
      night_index_runtime_(night_index_runtime),
      night_index_(night_index),
      summary_(summary),
      cache_fetch_(cache_fetch),
      result_cache_(result_cache),
      result_build_(result_build),
      range_plot_(range_plot),
      result_prepare_(result_prepare),
      build_queue_(build_queue) {}

bool ReportCacheMaintenanceService::active_work() const {
    return summary_.active() || cache_fetch_.active() ||
           result_build_.plot_active() || range_plot_.active();
}

bool ReportCacheMaintenanceService::clear_range(
    int64_t start_ms,
    int64_t end_ms,
    ReportCacheClearResult &out) {
    if (start_ms < 0 || end_ms <= start_ms) return false;

    ReportSummaryRecord *night_batch = nullptr;
    if (!summary_runtime_.take_scratch(portMAX_DELAY, night_batch)) return false;

    size_t night_count = 0;
    if (!summary_runtime_.take(portMAX_DELAY)) {
        summary_runtime_.give_scratch();
        return false;
    }

    const ReportSummaryRecord *records = summary_runtime_.records();
    const size_t record_count = summary_runtime_.record_count();
    for (size_t n = 0; records && n < record_count &&
                       n < AC_REPORT_SUMMARY_RECORD_MAX; ++n) {
        const ReportSummaryRecord &r = records[n];
        if (!r.valid) continue;

        const int64_t ns = static_cast<int64_t>(r.start_ms);
        const int64_t ne = static_cast<int64_t>(r.end_ms);
        if (ne <= ns || ns >= end_ms || ne <= start_ms) continue;
        if (night_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;

        night_batch[night_count++] = r;
    }
    summary_runtime_.give();

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

    summary_runtime_.give_scratch();
    return ok;
}

bool ReportCacheMaintenanceService::clear_all(ReportCacheClearResult &out) {
    out = {};
    if (active_work()) return false;

    uint32_t store_reset = 0;
    if (!ReportStore::reset_cache_store(store_reset)) {
        return false;
    }
    out.store_reset = store_reset;

    build_queue_.clear(0, true);
    result_cache_.invalidate(0, true);

    if (!summary_runtime_.take(portMAX_DELAY)) return false;
    summary_runtime_.clear_records();
    night_index_runtime_.clear_epochs();

    ReportSummaryStatus &status = summary_runtime_.status();
    status.state = ReportSummaryState::Idle;
    status.revision++;
    status.records_total = 0;
    status.nights_with_therapy = 0;
    status.elapsed_ms = 0;
    status.active_spool.clear();
    status.error.clear();
    summary_runtime_.give();

    summary_.request_json_snapshot_publish();
    range_plot_.reset(true);
    result_prepare_.clear_prepare();

    return true;
}

bool ReportCacheMaintenanceService::clear_night(uint64_t night_start_ms,
                                                ReportCacheClearResult &out) {
    out = {};
    if (active_work()) return false;

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
        night_index_.by_start(night_start_ms, *indexed_night) ==
            ReportNightIndexLookupResult::Ready &&
        indexed_night->summary.end_ms > indexed_night->summary.start_ms;
    const ReportSummaryRecord night = found ? indexed_night->summary
                                            : ReportSummaryRecord{};
    Memory::free(indexed_night);

    if (!found) return false;

    build_queue_.clear(night.start_ms, false);
    result_cache_.invalidate(night.start_ms, false);

    const bool ok = clear_range(static_cast<int64_t>(night.start_ms),
                                static_cast<int64_t>(night.end_ms),
                                out);

    uint32_t plot_deleted = 0;
    if (clear_result_plot_cache_for_night(night.start_ms, plot_deleted)) {
        out.plots_deleted += plot_deleted;
    }

    uint32_t result_json_deleted = 0;
    if (clear_result_json_cache_for_night(night.start_ms,
                                          result_json_deleted)) {
        out.result_json_deleted += result_json_deleted;
    }

    if (result_build_.status().night_start_ms == night.start_ms) {
        range_plot_.reset(true);
        result_prepare_.clear_prepare();
    }

    if (summary_runtime_.take(portMAX_DELAY)) {
        night_index_runtime_.remove_night(night.start_ms);
        summary_runtime_.give();
    }

    if (ok) out.nights_cleared = 1;
    return ok;
}

bool ReportCacheMaintenanceService::clear_oldest_nights(
    size_t max_nights,
    ReportCacheClearResult &out) {
    out = {};
    if (max_nights == 0) return true;
    if (active_work()) return false;

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
    if (!summary_runtime_.take(pdMS_TO_TICKS(20))) {
        Memory::free(snapshot);
        return false;
    }

    const ReportSummaryRecord *records = summary_runtime_.records();
    const size_t record_count = summary_runtime_.record_count();
    for (size_t i = 0; records && i < record_count &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &record = records[i];
        if (!record.valid || record.end_ms <= record.start_ms) continue;

        snapshot[count++] = record;
    }
    summary_runtime_.give();

    bool ok = true;
    const size_t limit = count < max_nights ? count : max_nights;
    for (size_t i = 0; i < limit; ++i) {
        ReportCacheClearResult current_clear;
        if (!clear_night(snapshot[i].start_ms, current_clear)) {
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

bool ReportCacheMaintenanceService::prune_to_latest_nights(
    size_t keep_latest,
    ReportCacheClearResult &out) {
    out = {};
    if (active_work()) return false;

    size_t report_nights = 0;
    if (!summary_runtime_.take(pdMS_TO_TICKS(20))) return false;

    const ReportSummaryRecord *records = summary_runtime_.records();
    const size_t record_count = summary_runtime_.record_count();
    for (size_t i = 0; records && i < record_count &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &record = records[i];
        if (!record.valid || record.end_ms <= record.start_ms) continue;

        report_nights++;
    }
    summary_runtime_.give();

    if (report_nights <= keep_latest) return true;
    return clear_oldest_nights(report_nights - keep_latest, out);
}

bool ReportManager::clear_cache_all(ReportCacheClearResult &out) {
    return cache_maintenance_.clear_all(out);
}

bool ReportManager::clear_cache_night(uint64_t night_start_ms,
                                      ReportCacheClearResult &out) {
    return cache_maintenance_.clear_night(night_start_ms, out);
}

bool ReportManager::clear_oldest_cache_nights(size_t max_nights,
                                              ReportCacheClearResult &out) {
    return cache_maintenance_.clear_oldest_nights(max_nights, out);
}

bool ReportManager::prune_cache_to_latest_nights(size_t keep_latest,
                                                 ReportCacheClearResult &out) {
    return cache_maintenance_.prune_to_latest_nights(keep_latest, out);
}

}  // namespace aircannect
