#include "report_manager.h"

#include "edf_report_catalog_job.h"
#include "report_index_scratch.h"

namespace aircannect {

bool ReportManager::edf_report_catalog_pending() const {
    if (!edf_catalog_) return false;

    EdfReportCatalogStatus status;
    if (!edf_catalog_->status(status, 0)) return true;
    if (status.state == EdfReportCatalogState::Ready ||
        status.state == EdfReportCatalogState::Error) {
        return false;
    }
    if (status.sessions > 0) {
        return false;
    }
    (void)edf_catalog_->request_refresh();
    return true;
}

bool ReportManager::prepare_result_by_therapy_index(size_t therapy_index,
                                                    bool refresh_cache) {
    const ResultPrepareOutcome outcome =
        prepare_result_by_therapy_index_internal(therapy_index,
                                                 refresh_cache);
    return outcome == ResultPrepareOutcome::Prepared ||
           outcome == ResultPrepareOutcome::Deferred;
}

bool ReportManager::request_result_prepare_by_therapy_index(
    size_t therapy_index,
    bool refresh_cache) {
    ScopedIndexedNight indexed_night("request_result_prepare_index");
    if (!indexed_night ||
        !indexed_night_by_therapy_index(therapy_index, indexed_night.get())) {
        return false;
    }

    if (refresh_cache) {
        invalidate_materialized(indexed_night->summary.start_ms, false);
    }
    const BuildQueueResult queued =
        enqueue_build(indexed_night->summary.start_ms,
                      therapy_index,
                      refresh_cache);
    return queued == BuildQueueResult::Queued ||
           queued == BuildQueueResult::AlreadyQueued;
}

bool ReportManager::request_result_prepare_by_start(uint64_t night_start_ms,
                                                    bool refresh_cache) {
    ScopedIndexedNight indexed_night("request_result_prepare_start_index");
    size_t therapy_index = 0;
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms,
                                indexed_night.get(),
                                &therapy_index)) {
        return false;
    }

    if (refresh_cache) {
        invalidate_materialized(indexed_night->summary.start_ms, false);
    }
    const BuildQueueResult queued =
        enqueue_build(indexed_night->summary.start_ms,
                      therapy_index,
                      refresh_cache);
    return queued == BuildQueueResult::Queued ||
           queued == BuildQueueResult::AlreadyQueued;
}

bool ReportManager::defer_result_prepare_for_summary(size_t therapy_index,
                                                     bool refresh_cache) {
    if (!summary_fetch_active_) return false;
    pending_result_prepare_ = true;
    pending_result_refresh_cache_ = refresh_cache;
    pending_result_therapy_index_ = therapy_index;
    clear_result_prepare();
    result_status_.state = ReportResultState::Preparing;
    result_status_.therapy_index = therapy_index;
    result_status_.error = "summary_fetching";
    return true;
}

bool ReportManager::load_result_night(size_t therapy_index,
                                      ReportSummaryRecord &night) {
    ScopedIndexedNight indexed_night("load_result_night_index");
    if (indexed_night &&
        indexed_night_by_therapy_index(therapy_index, indexed_night.get())) {
        night = indexed_night->summary;
        return true;
    }
    clear_result_prepare();
    fail_result_prepare("night_not_found");
    return false;
}


}  // namespace aircannect
