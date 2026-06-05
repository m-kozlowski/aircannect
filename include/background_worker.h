#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class ReportManager;
class RpcArbiter;
class ResmedOtaManager;
class OtaManager;

// What a single step() accomplished, so the worker can pace the next tick.
enum class JobStep {
    Idle,     // nothing to do now -> worker sleeps long
    Working,  // did a bounded unit, more to do -> short yield, re-tick
    Waiting,  // blocked on the main loop (e.g. a fetch) -> medium poll
};

class BackgroundJob {
public:
    virtual ~BackgroundJob() = default;
    virtual const char *name() const = 0;
    virtual JobStep step() = 0;
    virtual void on_preempt() {}
};

struct BackgroundWorkerStatus {
    bool task_started = false;
    bool enabled = false;
    bool idle = false;
    char gate_reason[16] = {};
    uint32_t ticks = 0;
    uint32_t stack_high_water_words = 0;
};

// Low-priority FreeRTOS task that runs registered jobs only while the device is
// idle (no therapy, stream, OTA, foreground report fetch, or recent web
// activity). One instance per device; see background_worker().
class BackgroundWorker {
public:
    void begin(ReportManager &report, RpcArbiter &arbiter,
               ResmedOtaManager &resmed_ota, OtaManager &ota);
    void add_job(BackgroundJob *job);

    void set_enabled(bool enabled) { enabled_.store(enabled); }
    bool enabled() const { return enabled_.load(); }

    // Push back the idle grace window so prefetch defers briefly after
    // foreground activity (e.g. a web request). Safe from any task.
    void note_activity();

    BackgroundWorkerStatus status() const;

private:
    static void task_entry(void *param);
    void run();
    bool gate_open(const char **reason) const;
    void publish(bool idle, const char *reason);

    ReportManager *report_ = nullptr;
    RpcArbiter *arbiter_ = nullptr;
    ResmedOtaManager *resmed_ota_ = nullptr;
    OtaManager *ota_ = nullptr;

    static constexpr size_t MAX_JOBS = 4;
    BackgroundJob *jobs_[MAX_JOBS] = {};
    size_t job_count_ = 0;

    std::atomic<bool> enabled_{true};
    volatile uint32_t last_activity_ms_ = 0;
    TaskHandle_t task_ = nullptr;

    mutable SemaphoreHandle_t status_lock_ = nullptr;
    BackgroundWorkerStatus status_ = {};
};

BackgroundWorker *background_worker();

}  // namespace aircannect
