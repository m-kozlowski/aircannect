#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

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
    virtual bool run_when_foreground_busy() const { return false; }
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
// idle (AS11 health known, no therapy, stream, OTA, foreground report fetch, or
// recent web activity). One instance per device; see background_worker().
class BackgroundWorker {
public:
    void begin();
    void add_job(BackgroundJob *job);

    // The main loop owns the gated subsystems; it publishes their state here
    // every iteration so the worker never reads them cross-task. Main loop only.
    void publish_gate(bool foreground_busy, bool as11_ready, bool stream_active,
                      bool resmed_ota_active, bool esp_ota_active,
                      bool therapy_running);

    void set_enabled(bool enabled) { enabled_.store(enabled); }
    bool enabled() const { return enabled_.load(); }

    // Push back the idle grace window so prefetch defers briefly after
    // foreground activity (e.g. a web request). Safe from any task.
    void note_activity();
    void wake();

    BackgroundWorkerStatus status() const;

private:
    static void task_entry(void *param);
    void run();
    bool gate_open(const char **reason) const;
    void publish(bool idle, const char *reason);

    // Gate-input bits published by the main loop (see publish_gate), packed into
    // one atomic so the worker reads a coherent snapshot lock-free.
    static constexpr uint32_t GATE_FOREGROUND = 1u << 0;
    static constexpr uint32_t GATE_STREAM = 1u << 1;
    static constexpr uint32_t GATE_RESMED_OTA = 1u << 2;
    static constexpr uint32_t GATE_ESP_OTA = 1u << 3;
    static constexpr uint32_t GATE_THERAPY = 1u << 4;
    static constexpr uint32_t GATE_AS11 = 1u << 5;
    static constexpr uint32_t GATE_UNPUBLISHED = 1u << 31;

    static constexpr size_t MAX_JOBS = 6;
    BackgroundJob *jobs_[MAX_JOBS] = {};
    size_t job_count_ = 0;

    std::atomic<bool> enabled_{true};
    std::atomic<uint32_t> last_activity_ms_{0};  // web grace; set from AsyncTCP
    // Gate inputs from the main loop; starts "unpublished" so the worker stays
    // gated until the first publish_gate().
    std::atomic<uint32_t> gate_inputs_{GATE_UNPUBLISHED};
    TaskHandle_t task_ = nullptr;

    mutable SemaphoreHandle_t status_lock_ = nullptr;
    BackgroundWorkerStatus status_ = {};
};

BackgroundWorker *background_worker();

}  // namespace aircannect
