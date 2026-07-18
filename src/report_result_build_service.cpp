#include "report_result_build_service.h"

#include "debug_log.h"
#include "report_daily_metrics.h"
#include "report_result_metrics.h"

namespace aircannect {
namespace {

constexpr uint32_t REPORT_STR_DURATION_TOLERANCE_MIN = 3;

}  // namespace

ReportResultBuildService::ReportResultBuildService(
    ReportResultCacheRuntime &cache,
    ReportEdfCatalogContext &edf_catalog)
    : cache_(cache),
      edf_catalog_(edf_catalog),
      plot_builder_(runtime_, cache_) {}

ReportResultStatus ReportResultBuildService::status() const {
    return runtime_.status_with_diagnostics(cache_);
}

bool ReportResultBuildService::ensure_chunks() {
    if (runtime_.ensure_chunks()) return true;

    fail_prepare("result_manifest_alloc_failed");
    return false;
}

void ReportResultBuildService::clear_prepare() {
    plot_builder_.reset();
    runtime_.clear_prepare_state();
}

void ReportResultBuildService::defer_prepare(uint64_t night_start_ms,
                                             size_t therapy_index,
                                             const char *message) {
    clear_prepare();

    ReportResultStatus &status = runtime_.status();
    status.state = ReportResultState::Preparing;
    status.therapy_index = therapy_index;
    status.night_start_ms = night_start_ms;
    status.error = message ? message : "night_index_busy";
}

void ReportResultBuildService::fail_prepare(const char *message) {
    plot_builder_.reset();
    runtime_.mark_error(message);

    if (runtime_.identity().night_start_ms() != 0 &&
        runtime_.status().night_start_ms != 0) {
        cache_.publish_result(runtime_);
    }

    runtime_.release_edf_sessions();

    Log::logf(CAT_REPORT, LOG_WARN, "Result prepare failed: %s\n",
              runtime_.status().error.c_str());
}

void ReportResultBuildService::begin_prepare_for_night(
    size_t therapy_index,
    const ReportIndexedNight &night,
    const char *current_etag) {
    clear_prepare();
    runtime_.status().state = ReportResultState::Preparing;
    runtime_.status().therapy_index = therapy_index;

    if (!runtime_.identity().set(night, current_etag)) {
        fail_prepare("result_identity_alloc_failed");
        return;
    }

    runtime_.set_ranges_from_indexed_night(night);
    runtime_.status().night_start_ms = night.summary.start_ms;
    runtime_.status().night_end_ms = night.summary.end_ms;
    runtime_.status().duration_min = night.summary.duration_min;

    ReportDailyMetrics metrics;
    if (report_daily_metrics_from_str_file(night.summary,
                                           REPORT_STR_DURATION_TOLERANCE_MIN,
                                           metrics)) {
        report_apply_daily_metrics_to_result_status(runtime_.status(),
                                                    metrics);
    }
    if (report_daily_metrics_from_summary(night.summary, metrics)) {
        report_apply_daily_metrics_to_result_status(runtime_.status(),
                                                    metrics);
    }
}

}  // namespace aircannect
