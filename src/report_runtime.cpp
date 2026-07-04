#include "report_manager.h"

#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "report_sources.h"
#include "report_store.h"
#include "spool_client.h"

namespace aircannect {

void ReportManager::poll(RpcArbiter &arbiter) {
    const bool realtime_active =
        arbiter.stream_realtime_active() ||
        arbiter.as11_state().therapy_state() == As11TherapyState::Running;

    // Foreground service points
    if (drain_source_events(arbiter)) return;

    service_build_queue(realtime_active);
    service_range_plot(realtime_active);
    service_prefetch(realtime_active);

    // EDF catalog changes invalidate summary-derived materialization.
    if (edf_catalog_) {
        EdfReportCatalogStatus catalog_status;

        if (edf_catalog_->status(catalog_status, 0) &&
            catalog_status.state == EdfReportCatalogState::Ready &&
            catalog_status.refresh_id != 0 &&
            catalog_status.refresh_id != edf_catalog_summary_refresh_id_) {
            invalidate_materialized(0, true);

            if (publish_summary_json_snapshot()) {
                edf_catalog_summary_refresh_id_ = catalog_status.refresh_id;
            }
        }
    }

    // Cross-task summary revision publication
    if (take_summary_lock(0)) {
        summary_revision_pub_.store(summary_status_.revision);
        give_summary_lock();
    }

    if (!spool_.active()) {
        observed_spool_rx_queue_full_alerts_ =
            arbiter.can_driver().stats().rx_queue_full_alerts;
    }

    // Idle trash cleanup
    if (!realtime_active &&
        !summary_fetch_active_ && !cache_fetch_active_ &&
        !plot_build_active_ && !range_build_active_ &&
        static_cast<int32_t>(millis() - next_trash_cleanup_ms_) >= 0) {
        next_trash_cleanup_ms_ = millis() + 250;

        uint32_t removed = 0;
        ReportStore::cleanup_trash_step(4, removed);
    }

    // Active cache fetch
    if (cache_fetch_active_) {
        poll_cache_fetch(arbiter);
    }

    // Active full-plot build
    if (plot_build_active_) {
        if (plot_build_idle_prebuild_) {
            const char *reason = "idle";

            if (realtime_active || !idle_prebuild_gate_open(&reason)) {
                const uint32_t elapsed_ms =
                    plot_build_started_ms_
                        ? static_cast<uint32_t>(millis() -
                                                plot_build_started_ms_)
                        : 0;
                Log::logf(CAT_REPORT,
                          LOG_DEBUG,
                          "Idle plot prebuild aborted reason=%s "
                          "night=%llu elapsed_ms=%lu\n",
                          realtime_active ? "realtime" :
                                            (reason ? reason : "gate"),
                          static_cast<unsigned long long>(
                              plot_build_night_start_ms_.load()),
                          static_cast<unsigned long>(elapsed_ms));
                reset_plot_build();
                release_result_edf_sessions();

                return;
            }
        }

        poll_result_plot_build();
        return;
    }

    // Active range-plot build
    if (range_build_active_) {
        if (realtime_active) return;

        poll_range_plot_build();
        return;
    }

    // Summary spool fetch
    if (!summary_fetch_active_) return;

    spool_.poll(arbiter);
    log_spool_can_pressure(arbiter);
    bool publish_progress = false;
    const uint32_t now_ms = millis();
    if (take_summary_lock(0)) {
        summary_status_.spool = spool_.status();
        summary_status_.elapsed_ms = summary_started_ms_
            ? now_ms - summary_started_ms_
            : 0;
        give_summary_lock();
        if (static_cast<int32_t>(now_ms - next_summary_progress_snapshot_ms_) >=
            0) {
            next_summary_progress_snapshot_ms_ = now_ms + 500;
            publish_progress = true;
        }
    }
    if (publish_progress) publish_summary_json_snapshot();

    if (spool_.complete()) {
        finish_summary_fetch();
    } else if (spool_.failed()) {
        fail_summary(spool_.status().error.c_str());
    }
}

bool ReportManager::handle_event(const RpcEvent &event) {
    if (cache_fetch_active_ && spool_.handle_event(event)) return true;
    if (summary_fetch_active_ && spool_.handle_event(event)) return true;
    return false;
}

bool ReportManager::drain_source_events(RpcArbiter &arbiter) {
    bool handled = false;
    for (size_t i = 0; i < AC_REPORT_SOURCE_EVENT_DRAIN_BUDGET; ++i) {
        RpcEvent event;
        if (!arbiter.next_source_event(RpcSource::Report, event)) break;
        (void)handle_event(event);
        handled = true;
    }
    return handled;
}

bool ReportManager::edf_catalog_status(EdfReportCatalogStatus &out,
                                       uint32_t timeout_ms) const {
    return edf_catalog_ && edf_catalog_->status(out, timeout_ms);
}

bool ReportManager::foreground_busy() const {
    if (!cache_fetch_active_) return false;

    // A cache fetch is in flight: it's foreground unless it's the prefetch's own.
    if (!prefetch_lock_) return true;

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const bool prefetch_owned = (prefetch_phase_ == PrefetchPhase::Fetching);
    xSemaphoreGive(prefetch_lock_);

    return !prefetch_owned;
}

bool ReportManager::background_work_active() const {
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return true;
    }
    if (build_queue_lock_) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
        const bool queued = build_queue_count_ > 0;
        xSemaphoreGive(build_queue_lock_);
        if (queued) return true;
    }
    if (!prefetch_lock_) return false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const PrefetchPhase phase = prefetch_phase_;
    xSemaphoreGive(prefetch_lock_);
    return phase == PrefetchPhase::Selecting ||
           phase == PrefetchPhase::Pending ||
           phase == PrefetchPhase::Fetching ||
           phase == PrefetchPhase::Done;
}

void ReportManager::log_spool_can_pressure(const RpcArbiter &arbiter) {
    const uint32_t alerts =
        arbiter.can_driver().stats().rx_queue_full_alerts;
    if (alerts == observed_spool_rx_queue_full_alerts_) return;
    observed_spool_rx_queue_full_alerts_ = alerts;
    if (!spool_.active()) return;

    const SpoolClientStatus &spool = spool_.status();
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "spool CAN RX pressure source=%s state=%s round=%u "
              "spool_id=%lu round_fragments=%lu round_bytes=%lu "
              "total_fragments=%lu total_bytes=%lu alerts=%lu\n",
              spool.spool_type.c_str(),
              spool_client_state_name(spool.state),
              static_cast<unsigned>(spool.current_round),
              static_cast<unsigned long>(spool.active_spool_id),
              static_cast<unsigned long>(spool.round_fragments),
              static_cast<unsigned long>(spool.round_bytes),
              static_cast<unsigned long>(spool.fragments),
              static_cast<unsigned long>(spool.bytes),
              static_cast<unsigned long>(alerts));
}

bool ReportManager::cancel_cache_fetch() {
    if (!cache_fetch_active_) return false;
    spool_.reset();
    abort_cache_write_fetch();
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error = "cancelled";
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Cache fetch cancelled night=%llu source=%s\n",
              static_cast<unsigned long long>(cache_status_.night_start_ms),
              report_source_spool_type(cache_status_.active_source));
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare("cache_cancelled");
    }
    return true;
}

}  // namespace aircannect
