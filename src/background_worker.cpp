#include "background_worker.h"

#include "board.h"
#include "debug_log.h"

namespace aircannect {
namespace {
BackgroundWorker *g_instance = nullptr;
}

BackgroundWorker *background_worker() { return g_instance; }

void BackgroundWorker::begin() {
    if (!status_lock_) status_lock_ = xSemaphoreCreateMutex();
    g_instance = this;
    if (task_) return;
    xTaskCreatePinnedToCore(task_entry, "ac_bgworker",
                            AC_BG_WORKER_TASK_STACK, this,
                            AC_BG_WORKER_TASK_PRIO, &task_,
                            AC_BG_WORKER_TASK_CORE);
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[BGWORKER] started core=%u prio=%u stack=%u\n",
              static_cast<unsigned>(AC_BG_WORKER_TASK_CORE),
              static_cast<unsigned>(AC_BG_WORKER_TASK_PRIO),
              static_cast<unsigned>(AC_BG_WORKER_TASK_STACK));
}

void BackgroundWorker::add_job(BackgroundJob *job) {
    if (!job || job_count_ >= MAX_JOBS) return;
    jobs_[job_count_++] = job;
}

void BackgroundWorker::note_activity() {
    uint32_t now = millis();
    if (now == 0) now = 1;  // 0 is the "never" sentinel
    last_activity_ms_.store(now);
}

void BackgroundWorker::publish_gate(bool foreground_busy, bool stream_active,
                                   bool resmed_ota_active, bool esp_ota_active,
                                   bool therapy_running) {
    uint32_t v = 0;
    if (foreground_busy) v |= GATE_FOREGROUND;
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
    status_.stack_high_water_words = uxTaskGetStackHighWaterMark(nullptr);
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
        const char *reason = "idle";
        const bool open = gate_open(&reason);
        // reason is always a string literal, so pointer compare detects changes.
        if (reason != last_reason) {
            Log::logf(CAT_GENERAL, LOG_INFO, "[BGWORKER] gate=%s\n", reason);
            last_reason = reason;
        }
        publish(open, reason);

        if (!open) {
            for (size_t i = 0; i < job_count_; ++i) jobs_[i]->on_preempt();
            vTaskDelay(pdMS_TO_TICKS(AC_BG_WORKER_BUSY_RECHECK_MS));
            continue;
        }

        JobStep result = JobStep::Idle;
        for (size_t i = 0; i < job_count_; ++i) {
            const char *r = "idle";
            if (!gate_open(&r)) break;  // foreground appeared mid-pass
            const JobStep s = jobs_[i]->step();
            if (s != JobStep::Idle) {
                result = s;
                break;  // one active job per pass, then re-gate
            }
        }

        if (result == JobStep::Idle && (++idle_ticks % 30) == 0) {
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[BGWORKER] heartbeat idle jobs=%u\n",
                      static_cast<unsigned>(job_count_));
        }
        uint32_t delay = AC_BG_WORKER_IDLE_TICK_MS;
        if (result == JobStep::Working) delay = AC_BG_WORKER_WORK_TICK_MS;
        else if (result == JobStep::Waiting) delay = AC_BG_WORKER_BUSY_RECHECK_MS;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

}  // namespace aircannect
