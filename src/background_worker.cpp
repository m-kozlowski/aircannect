#include "background_worker.h"

#include <string.h>

#include "board.h"
#include "debug_log.h"

namespace aircannect {
namespace {
BackgroundWorker *g_instance = nullptr;
}

BackgroundWorker *background_worker() { return g_instance; }

void BackgroundWorker::begin() {
    if (!status_lock_) {
        status_lock_ = xSemaphoreCreateMutexStatic(&status_lock_storage_);
    }
    g_instance = this;
    if (task_) return;

    uint32_t startup_grace_until =
        millis() + AC_BG_WORKER_STARTUP_GRACE_MS;
    if (startup_grace_until == 0) startup_grace_until = 1;
    startup_grace_until_ms_.store(startup_grace_until);

    xTaskCreatePinnedToCore(task_entry, "ac_bgworker",
                            AC_BG_WORKER_TASK_STACK, this,
                            AC_BG_WORKER_TASK_PRIO, &task_,
                            AC_BG_WORKER_TASK_CORE);
    Log::logf(CAT_BGWORKER, LOG_INFO,
              "started core=%u prio=%u stack=%u\n",
              static_cast<unsigned>(AC_BG_WORKER_TASK_CORE),
              static_cast<unsigned>(AC_BG_WORKER_TASK_PRIO),
              static_cast<unsigned>(AC_BG_WORKER_TASK_STACK));
}

bool BackgroundWorker::add_job(BackgroundJob *job) {
    if (!job) {
        Log::logf(CAT_BGWORKER, LOG_ERROR,
                  "job registration failed: null job\n");
        return false;
    }
    if (job_count_ >= MAX_JOBS) {
        Log::logf(CAT_BGWORKER, LOG_ERROR,
                  "job registration failed: capacity=%u dropped=%s\n",
                  static_cast<unsigned>(MAX_JOBS),
                  job->name() ? job->name() : "?");
        return false;
    }
    jobs_[job_count_++] = job;
    return true;
}

#if AC_STACK_PROFILE_ENABLED
uint32_t BackgroundWorker::stack_high_water_bytes() const {
    return task_ ? uxTaskGetStackHighWaterMark(task_) : 0;
}
#endif

void BackgroundWorker::note_activity() {
    uint32_t now = millis();
    if (now == 0) now = 1;  // 0 is the "never" sentinel
    last_activity_ms_.store(now);
    wake();
}

void BackgroundWorker::wake() {
    if (task_) xTaskNotifyGive(task_);
}

bool BackgroundWorker::idle_gate_open(const char **reason) const {
    const char *local_reason = "idle";
    const bool open = gate_open(&local_reason);
    if (reason) *reason = local_reason;
    return open;
}

void BackgroundWorker::publish_gate(bool foreground_busy, bool as11_ready,
                                   bool stream_active,
                                   bool resmed_ota_active, bool esp_ota_active,
                                   bool therapy_running) {
    uint32_t v = 0;
    if (foreground_busy) v |= GATE_FOREGROUND;
    if (!as11_ready) v |= GATE_AS11;
    if (stream_active) v |= GATE_STREAM;
    if (resmed_ota_active) v |= GATE_RESMED_OTA;
    if (esp_ota_active) v |= GATE_ESP_OTA;
    if (therapy_running) v |= GATE_THERAPY;
    gate_inputs_.store(v);
}

bool BackgroundWorker::gate_open(const char **reason) const {
    if (!enabled_.load()) { *reason = "disabled"; return false; }
    // The main loop owns these subsystems and publishes their state via
    // publish_gate(); the worker reads the snapshot, never the managers.
    const uint32_t gi = gate_inputs_.load();
    if (gi & GATE_UNPUBLISHED) { *reason = "starting"; return false; }
    // A user-initiated report op owns the spool/caches (the prefetch's own fetch
    // is excluded upstream in foreground_busy()).
    if (gi & GATE_FOREGROUND) { *reason = "report_busy"; return false; }
    // Background AS11 fetches should not be the first traffic after boot. Let
    // the main health lane establish identity/status first.
    if (gi & GATE_AS11) { *reason = "as11"; return false; }
    // CAN must stay free for live streaming and therapy capture.
    if (gi & GATE_STREAM) { *reason = "stream"; return false; }
    if (gi & GATE_RESMED_OTA) { *reason = "resmed_ota"; return false; }
    if (gi & GATE_ESP_OTA) { *reason = "esp_ota"; return false; }
    if (gi & GATE_THERAPY) { *reason = "therapy"; return false; }
    const uint32_t last = last_activity_ms_.load();
    if (last != 0 && (millis() - last) < AC_BG_WORKER_ACTIVITY_GRACE_MS) {
        *reason = "web_grace";
        return false;
    }

    const uint32_t startup_grace_until = startup_grace_until_ms_.load();
    if (startup_grace_until != 0 &&
        static_cast<int32_t>(millis() - startup_grace_until) < 0) {
        *reason = "starting";
        return false;
    }

    *reason = "idle";
    return true;
}

void BackgroundWorker::publish(bool idle, const char *reason) {
    if (!status_lock_ || !xSemaphoreTake(status_lock_, pdMS_TO_TICKS(10))) {
        return;
    }
    status_.task_started = true;
    status_.enabled = enabled_.load();
    status_.idle = idle;
    snprintf(status_.gate_reason, sizeof(status_.gate_reason), "%s",
             reason ? reason : "");
    status_.ticks++;
#if AC_STACK_PROFILE_ENABLED
    status_.stack_high_water_words = uxTaskGetStackHighWaterMark(nullptr);
#endif
    xSemaphoreGive(status_lock_);
}

BackgroundWorkerStatus BackgroundWorker::status() const {
    BackgroundWorkerStatus out;
    if (status_lock_ && xSemaphoreTake(status_lock_, pdMS_TO_TICKS(10))) {
        out = status_;
        xSemaphoreGive(status_lock_);
    }
    out.enabled = enabled_.load();  // live toggle state, not last-published
    return out;
}

void BackgroundWorker::task_entry(void *param) {
    static_cast<BackgroundWorker *>(param)->run();
}

void BackgroundWorker::run() {
    const char *last_reason = "";
    uint32_t idle_ticks = 0;

    for (;;) {
        // Gate snapshot
        const char *reason = "idle";
        const bool open = gate_open(&reason);

        if (reason != last_reason) {
            Log::logf(CAT_BGWORKER, LOG_DEBUG, "gate=%s\n", reason);
            last_reason = reason;
        }

        publish(open, reason);

        // Gate-closed jobs
        if (!open) {
            JobStep foreground_result = JobStep::Idle;
            const bool foreground_busy =
                reason && strcmp(reason, "report_busy") == 0;

            for (size_t i = 0; i < job_count_; ++i) {
                if (jobs_[i]->run_when_gate_closed(reason)) continue;

                if (!foreground_busy ||
                    !jobs_[i]->run_when_foreground_busy()) {
                    jobs_[i]->on_preempt();
                }
            }

            if (foreground_busy) {
                if (foreground_cursor_ >= job_count_) foreground_cursor_ = 0;

                const size_t start = foreground_cursor_;

                for (size_t n = 0; n < job_count_; ++n) {
                    const size_t i = (start + n) % job_count_;
                    if (!jobs_[i]->run_when_foreground_busy()) continue;

                    foreground_cursor_ = (i + 1) % job_count_;
                    const JobStep s = jobs_[i]->step();

                    if (s != JobStep::Idle) {
                        foreground_result = s;
                        break;
                    }
                }
            }

            if (gate_closed_cursor_ >= job_count_) gate_closed_cursor_ = 0;

            const size_t gate_start = gate_closed_cursor_;

            for (size_t n = 0; n < job_count_; ++n) {
                if (foreground_result != JobStep::Idle) break;

                const size_t i = (gate_start + n) % job_count_;
                if (!jobs_[i]->run_when_gate_closed(reason)) continue;

                gate_closed_cursor_ = (i + 1) % job_count_;
                const JobStep s = jobs_[i]->step_when_gate_closed(reason);

                if (s != JobStep::Idle) {
                    foreground_result = s;
                    break;
                }
            }

            if (foreground_result != JobStep::Idle) {
                const uint32_t delay =
                    foreground_result == JobStep::Working
                        ? AC_BG_WORKER_WORK_TICK_MS
                        : AC_BG_WORKER_BUSY_RECHECK_MS;

                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay));
                continue;
            }

            ulTaskNotifyTake(pdTRUE,
                             pdMS_TO_TICKS(AC_BG_WORKER_BUSY_RECHECK_MS));
            continue;
        }

        // Priority drains
        JobStep result = JobStep::Idle;

        if (drain_cursor_ >= job_count_) drain_cursor_ = 0;

        const size_t drain_start = drain_cursor_;

        for (size_t n = 0; n < job_count_; ++n) {
            const char *r = "idle";
            if (!gate_open(&r)) break;  // foreground appeared mid-pass

            const size_t i = (drain_start + n) % job_count_;
            if (!jobs_[i]->drain_before_regular_jobs()) continue;

            drain_cursor_ = (i + 1) % job_count_;
            const JobStep s = jobs_[i]->step();

            if (s != JobStep::Idle) {
                result = s;
                break;
            }
        }

        // "Drain before" is priority, not exclusivity. Long/cache-backed
        // drains can otherwise starve regular dependency jobs such as the EDF
        // report catalog and leave foreground report requests stuck waiting for
        // state that only the worker can build.

        // Regular jobs
        if (regular_cursor_ >= job_count_) regular_cursor_ = 0;

        const size_t regular_start = regular_cursor_;
        JobStep regular_result = JobStep::Idle;

        for (size_t n = 0; n < job_count_; ++n) {
            const char *r = "idle";
            if (!gate_open(&r)) break;  // foreground appeared mid-pass

            const size_t i = (regular_start + n) % job_count_;
            if (jobs_[i]->drain_before_regular_jobs()) continue;

            regular_cursor_ = (i + 1) % job_count_;
            const JobStep s = jobs_[i]->step();

            if (s != JobStep::Idle) {
                regular_result = s;
                break;  // one active job per pass, then re-gate
            }
        }

        if (regular_result == JobStep::Working ||
            (regular_result == JobStep::Waiting && result == JobStep::Idle)) {
            result = regular_result;
        }

        // Backoff
        if (result == JobStep::Idle && (++idle_ticks % 30) == 0) {
            Log::logf(CAT_BGWORKER, LOG_DEBUG,
                      "heartbeat idle jobs=%u\n",
                      static_cast<unsigned>(job_count_));
        }

        uint32_t delay = AC_BG_WORKER_IDLE_TICK_MS;
        if (result == JobStep::Working) delay = AC_BG_WORKER_WORK_TICK_MS;
        else if (result == JobStep::Waiting) {
            delay = AC_BG_WORKER_BUSY_RECHECK_MS;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay));
    }
}

}  // namespace aircannect
