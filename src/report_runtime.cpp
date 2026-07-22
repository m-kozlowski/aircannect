#include "report_manager.h"

#include <Arduino.h>

#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "report_build_queue_service.h"
#include "report_cache_fetch_service.h"
#include "report_cache_storage_runtime.h"
#include "report_edf_catalog_context.h"
#include "report_fetch_runtime.h"
#include "report_plot_prebuild_service.h"
#include "report_prefetch_service.h"
#include "report_range_plot_builder.h"
#include "report_result_build_service.h"
#include "report_result_cache_runtime.h"
#include "report_result_prepare_service.h"
#include "report_runtime_service.h"
#include "report_summary_runtime.h"
#include "report_summary_service.h"
#include "rpc_arbiter.h"

namespace aircannect {
namespace {

const char *result_prepare_outcome_name(
    ReportResultPrepareService::ResultPrepareOutcome outcome) {
    switch (outcome) {
        case ReportResultPrepareService::ResultPrepareOutcome::Prepared:
            return "prepared";
        case ReportResultPrepareService::ResultPrepareOutcome::Deferred:
            return "deferred";
        case ReportResultPrepareService::ResultPrepareOutcome::Retry:
            return "retry";
        case ReportResultPrepareService::ResultPrepareOutcome::Failed:
            return "failed";
    }

    return "unknown";
}

uint32_t summary_publish_retry_delay_ms(uint16_t failures) {
    static constexpr uint32_t RETRY_DELAYS_MS[] = {
        1000,
        2000,
        5000,
        10000,
        30000,
    };

    const size_t last =
        sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]) - 1;
    const size_t index = failures > 0 ? failures - 1 : 0;
    return RETRY_DELAYS_MS[index < last ? index : last];
}

}  // namespace

ReportRuntimeService::ReportRuntimeService(
    ReportFetchRuntime &fetch,
    ReportSummaryRuntime &summary_runtime,
    ReportCacheStorageRuntime &cache_storage,
    ReportResultCacheRuntime &result_cache,
    ReportSummaryService &summary,
    ReportCacheFetchService &cache_fetch,
    ReportResultBuildService &result_build,
    ReportRangePlotBuilder &range_plot,
    ReportResultPrepareService &result_prepare,
    ReportPlotPrebuildService &plot_prebuild,
    ReportBuildQueueService &build_queue,
    ReportPrefetchService &prefetch,
    ReportEdfCatalogContext &edf_catalog)
    : fetch_(fetch),
      summary_runtime_(summary_runtime),
      cache_storage_(cache_storage),
      result_cache_(result_cache),
      summary_(summary),
      cache_fetch_(cache_fetch),
      result_build_(result_build),
      range_plot_(range_plot),
      result_prepare_(result_prepare),
      plot_prebuild_(plot_prebuild),
      build_queue_(build_queue),
      prefetch_(prefetch),
      edf_catalog_(edf_catalog) {}

void ReportRuntimeService::poll(RpcArbiter &arbiter,
                                bool therapy_running) {
    const bool realtime_active =
        arbiter.stream_realtime_active() || therapy_running;

    // Foreground service points
    if (drain_source_events(arbiter)) return;

    service_build_queue(realtime_active);
    service_range_plot(realtime_active);
    prefetch_.service(realtime_active);

    // Summary and EDF catalog changes republish one coherent night snapshot.
    service_summary_snapshot_publish();

    // Cross-task summary revision publication
    if (summary_runtime_.take(0)) {
        summary_runtime_.publish_revision();
        summary_runtime_.give();
    }

    fetch_.observe_idle(arbiter);

    cache_storage_.service_trash_cleanup(realtime_active, busy());

    // Active cache fetch
    if (cache_fetch_.active()) {
        handle_cache_fetch_event(cache_fetch_.poll(arbiter));
    }

    // Active full-plot build
    ReportResultPlotBuilder &plot_builder = result_build_.plot_builder();
    if (plot_builder.active()) {
        if (plot_builder.idle_prebuild_active()) {
            const char *reason = "idle";

            if (realtime_active || !plot_prebuild_.gate_open(&reason)) {
                plot_builder.abort_idle_prebuild(
                    realtime_active ? "realtime" : reason);
                return;
            }
        }

        plot_builder.poll();
        return;
    }

    // Active range-plot build
    if (range_plot_.active()) {
        if (realtime_active) return;

        range_plot_.poll();
        return;
    }

    handle_summary_fetch_event(summary_.poll(arbiter));
}

void ReportRuntimeService::service_summary_snapshot_publish() {
    EdfReportCatalogStatus catalog_status;
    const uint32_t now_ms = millis();
    if (edf_catalog_ &&
        edf_catalog_.ready_refresh_changed(catalog_status)) {
        if (!summary_publish_retry_.catalog_pending ||
            summary_publish_retry_.catalog_refresh_id !=
                catalog_status.refresh_id) {
            summary_publish_retry_ = {};
            summary_publish_retry_.catalog_pending = true;
            summary_publish_retry_.catalog_refresh_id =
                catalog_status.refresh_id;
            summary_publish_retry_.catalog_sessions =
                catalog_status.sessions;
            summary_.request_json_snapshot_publish();
        }

        if (!summary_publish_retry_.cache_invalidated) {
            result_cache_.invalidate(0, true);
            summary_publish_retry_.cache_invalidated = true;
        }
    }

    if (!summary_.json_snapshot_publish_pending()) {
        if (summary_publish_retry_.catalog_pending) {
            edf_catalog_.mark_summary_published(
                summary_publish_retry_.catalog_refresh_id);
            summary_publish_retry_ = {};
        }
        return;
    }

    const uint32_t snapshot_generation =
        summary_.json_snapshot_generation();
    if (summary_publish_retry_.snapshot_generation !=
        snapshot_generation) {
        summary_publish_retry_.snapshot_generation = snapshot_generation;
        summary_publish_retry_.next_attempt_ms = 0;
        summary_publish_retry_.next_warning_ms = 0;
        summary_publish_retry_.failures = 0;
        summary_publish_retry_.busy_retries = 0;
    }

    if (summary_publish_retry_.next_attempt_ms != 0 &&
        static_cast<int32_t>(now_ms -
                             summary_publish_retry_.next_attempt_ms) < 0) {
        return;
    }

    const uint32_t publish_start_ms = millis();
    const ReportSummarySnapshotResult result =
        summary_.publish_json_snapshot();
    const uint32_t publish_ms =
        static_cast<uint32_t>(millis() - publish_start_ms);

    if (result == ReportSummarySnapshotResult::Published) {
        const bool catalog_pending =
            summary_publish_retry_.catalog_pending;
        const log_level_t level = summary_publish_retry_.failures > 0 ||
                                      summary_publish_retry_.busy_retries > 0 ||
                                      !catalog_pending || publish_ms > 500
            ? LOG_INFO
            : LOG_DEBUG;

        if (catalog_pending) {
            Log::logf(
                CAT_REPORT,
                level,
                "Summary snapshot published refresh_id=%lu sessions=%lu "
                "generation=%lu ms=%lu retries=%u\n",
                static_cast<unsigned long>(
                    summary_publish_retry_.catalog_refresh_id),
                static_cast<unsigned long>(
                    summary_publish_retry_.catalog_sessions),
                static_cast<unsigned long>(snapshot_generation),
                static_cast<unsigned long>(publish_ms),
                static_cast<unsigned>(summary_publish_retry_.failures +
                                      summary_publish_retry_.busy_retries));

            edf_catalog_.mark_summary_published(
                summary_publish_retry_.catalog_refresh_id);
        } else {
            Log::logf(CAT_REPORT,
                      level,
                      "Summary snapshot recovered generation=%lu ms=%lu "
                      "retries=%u\n",
                      static_cast<unsigned long>(snapshot_generation),
                      static_cast<unsigned long>(publish_ms),
                      static_cast<unsigned>(
                          summary_publish_retry_.failures +
                          summary_publish_retry_.busy_retries));
        }

        summary_publish_retry_ = {};
        return;
    }

    if (result == ReportSummarySnapshotResult::Busy) {
        summary_publish_retry_.busy_retries++;
        summary_publish_retry_.next_attempt_ms = now_ms + 50;

        if (summary_publish_retry_.next_warning_ms == 0) {
            summary_publish_retry_.next_warning_ms = now_ms + 5000;
            return;
        }
        if (static_cast<int32_t>(
                now_ms - summary_publish_retry_.next_warning_ms) < 0) {
            return;
        }

        summary_publish_retry_.next_warning_ms = now_ms + 60000;
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Summary snapshot still busy generation=%lu error=%s "
                  "retries=%u\n",
                  static_cast<unsigned long>(snapshot_generation),
                  summary_.snapshot_error(),
                  static_cast<unsigned>(
                      summary_publish_retry_.busy_retries));
        return;
    }

    summary_publish_retry_.failures++;
    summary_publish_retry_.next_attempt_ms =
        now_ms + summary_publish_retry_delay_ms(
                     summary_publish_retry_.failures);

    const bool warning_due = summary_publish_retry_.failures == 1 ||
        summary_publish_retry_.next_warning_ms == 0 ||
        static_cast<int32_t>(now_ms -
                             summary_publish_retry_.next_warning_ms) >= 0;
    if (!warning_due) return;

    summary_publish_retry_.next_warning_ms = now_ms + 60000;
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "Summary snapshot publish failed generation=%lu "
              "refresh_id=%lu error=%s ms=%lu attempt=%u retry_ms=%lu\n",
              static_cast<unsigned long>(snapshot_generation),
              static_cast<unsigned long>(
                  summary_publish_retry_.catalog_refresh_id),
              summary_.snapshot_error(),
              static_cast<unsigned long>(publish_ms),
              static_cast<unsigned>(summary_publish_retry_.failures),
              static_cast<unsigned long>(
                  summary_publish_retry_delay_ms(
                      summary_publish_retry_.failures)));
}

bool ReportRuntimeService::handle_event(const RpcEvent &event) {
    return fetch_.handle_event(event);
}

bool ReportRuntimeService::drain_source_events(RpcArbiter &arbiter) {
    bool handled = false;
    for (size_t i = 0; i < AC_REPORT_SOURCE_EVENT_DRAIN_BUDGET; ++i) {
        RpcEvent event;
        if (!arbiter.next_source_event(RpcSource::Report, event)) break;
        (void)handle_event(event);
        handled = true;
    }
    return handled;
}

void ReportRuntimeService::service_build_queue(bool realtime_active) {
    // Existing report work owns the materialization pipeline.
    if (result_build_.plot_builder().active()) {
        if (result_build_.plot_builder().idle_prebuild_active() &&
            build_queue_.has_foreground_pending()) {
            result_build_.plot_builder().preempt_idle_prebuild();
        } else {
            build_queue_.note_service_block("plot");
            return;
        }
    }

    if (summary_.active() || range_plot_.active()) {
        const char *reason = "range";
        if (summary_.active()) {
            reason = "summary";
        }

        build_queue_.note_service_block(reason);
        return;
    }

    // Realtime stream/therapy work preempts report materialization.
    if (realtime_active) {
        build_queue_.note_service_block("realtime");
        return;
    }

    ReportBuildQueueService::ResultBuildJob job;
    const uint32_t now_ms = millis();
    const auto selection = build_queue_.select_next(now_ms, job);
    if (selection ==
        ReportBuildQueueService::BuildQueueSelection::Empty) {
        return;
    }
    if (selection ==
        ReportBuildQueueService::BuildQueueSelection::Waiting) {
        build_queue_.note_service_block("retry_wait");
        return;
    }

    if (job.idle_prebuild) {
        const char *reason = "idle";
        if (!plot_prebuild_.gate_open(&reason)) {
            build_queue_.note_service_block(reason ? reason : "gate");
            return;
        }
    }

    if (cache_fetch_.active()) {
        if (!job.idle_prebuild) prefetch_.yield_to_foreground();

        if (cache_fetch_.active()) {
            build_queue_.note_service_block("cache_fetch");
            return;
        }
    }

    build_queue_.note_service_started();

    ReportResultRuntime &runtime = result_build_.runtime();
    runtime.plot().active_idle_prebuild = job.idle_prebuild;

    const auto outcome =
        result_prepare_.prepare_by_night_start(job.night_start_ms,
                                               job.therapy_index,
                                               job.refresh);

    runtime.plot().active_idle_prebuild = false;

    const char *outcome_name = result_prepare_outcome_name(outcome);
    const ReportResultStatus &status = runtime.status();
    const char *error = status.error.length() ? status.error.c_str() : "--";

    build_queue_.note_build_result(job,
                                   outcome_name,
                                   runtime.state_name(),
                                   status.error.c_str());

    const bool failed =
        outcome == ReportResultPrepareService::ResultPrepareOutcome::Failed;
    Log::logf(CAT_REPORT,
              failed ? LOG_WARN : LOG_DEBUG,
              "Result build step night=%llu index=%lu refresh=%u "
              "idle_prebuild=%u outcome=%s state=%s error=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long long>(job.night_start_ms),
              static_cast<unsigned long>(job.therapy_index),
              job.refresh ? 1u : 0u,
              job.idle_prebuild ? 1u : 0u,
              outcome_name,
              runtime.state_name(),
              error,
              static_cast<unsigned long>(status.chunk_count),
              static_cast<unsigned long>(status.record_count),
              static_cast<unsigned long>(status.payload_bytes));

    if (outcome ==
            ReportResultPrepareService::ResultPrepareOutcome::Deferred ||
        outcome == ReportResultPrepareService::ResultPrepareOutcome::Retry) {
        const bool retry = outcome ==
            ReportResultPrepareService::ResultPrepareOutcome::Retry;
        const auto defer_result = build_queue_.defer(job, retry, millis());
        if (defer_result ==
            ReportBuildQueueService::BuildQueueDeferResult::RetryExhausted) {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "Result build retries exhausted night=%llu index=%lu "
                      "attempts=%u error=%s\n",
                      static_cast<unsigned long long>(job.night_start_ms),
                      static_cast<unsigned long>(job.therapy_index),
                      static_cast<unsigned>(job.retry_attempts),
                      error);
            result_prepare_.fail_prepare("prepare_retry_exhausted");
            build_queue_.note_build_result(
                job,
                "failed",
                runtime.state_name(),
                runtime.status().error.c_str());
        }
        return;
    }

    build_queue_.remove(job);
}

void ReportRuntimeService::handle_cache_fetch_event(
    ReportCacheFetchEvent event) {
    if (event == ReportCacheFetchEvent::None) return;

    ReportPendingResultPrepare pending;
    if (!cache_fetch_.take_pending_prepare(pending)) return;

    if (event == ReportCacheFetchEvent::Completed) {
        result_prepare_.prepare_by_therapy_index(pending.therapy_index, false);
        return;
    }

    result_prepare_.fail_prepare(cache_fetch_.status().error.c_str());
}

void ReportRuntimeService::handle_summary_fetch_event(
    ReportSummaryFetchEvent event) {
    if (event == ReportSummaryFetchEvent::None) return;

    ReportPendingResultPrepare pending;
    if (!cache_fetch_.take_pending_prepare(pending)) return;

    if (event == ReportSummaryFetchEvent::Completed) {
        result_prepare_.prepare_by_therapy_index(pending.therapy_index,
                                                 pending.refresh_cache);
        return;
    }

    const ReportSummaryStatus status = summary_.status();
    result_prepare_.fail_prepare(status.error.c_str());
}

bool ReportRuntimeService::busy() const {
    return summary_.active() || cache_fetch_.active() ||
           result_build_.plot_active() || range_plot_.active();
}

bool ReportRuntimeService::foreground_busy() const {
    return result_cache_.loader_active() || prefetch_.foreground_busy();
}

bool ReportRuntimeService::background_work_active() const {
    if (busy()) return true;
    if (build_queue_.has_pending()) return true;

    return prefetch_.work_active();
}

bool ReportRuntimeService::cancel_cache_fetch() {
    if (!cache_fetch_.active()) return false;
    handle_cache_fetch_event(cache_fetch_.cancel("cancelled"));
    return true;
}

void ReportManager::poll(RpcArbiter &arbiter, bool therapy_running) {
    runtime_service_.poll(arbiter, therapy_running);
}

bool ReportManager::handle_event(const RpcEvent &event) {
    return runtime_service_.handle_event(event);
}

bool ReportManager::edf_catalog_status(EdfReportCatalogStatus &out,
                                       uint32_t timeout_ms) const {
    return edf_catalog_ && edf_catalog_.status(out, timeout_ms);
}

bool ReportManager::foreground_busy() const {
    return runtime_service_.foreground_busy();
}

bool ReportManager::background_work_active() const {
    return runtime_service_.background_work_active();
}

bool ReportManager::cancel_cache_fetch() {
    return runtime_service_.cancel_cache_fetch();
}

}  // namespace aircannect
