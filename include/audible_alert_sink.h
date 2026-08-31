#pragma once

#include <atomic>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "alert_manager.h"
#include "audible_output.h"

namespace aircannect {

class AudibleAlertSink final : public AlertSink {
public:
    bool begin(AudibleOutput &output, uint32_t now_ms);
    void poll(uint32_t now_ms);
    void set_enabled(bool enabled);
    void set_volume(uint8_t percent);
    void accept(const AlertEvent &event) override;

private:
    static void task_entry(void *context);
    void task_loop();
    bool start();
    bool active() const;

    AudibleOutput *output_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint32_t next_start_ms_ = 0;
    std::atomic<uint32_t> active_alerts_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<uint8_t> volume_percent_{100};
};

}  // namespace aircannect
