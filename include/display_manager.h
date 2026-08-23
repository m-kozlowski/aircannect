#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <stdint.h>

#include "display_snapshot.h"

namespace aircannect {

class DisplayDevice;

class DisplayManager {
public:
    bool begin();
    void publish(const DisplaySnapshot &snapshot);
    void publish_pressure(const DisplayPressureSnapshot &pressure);

    bool available() const { return device_ != nullptr; }
    bool backlight_on() const { return backlight_requested_.load(); }
    void toggle_backlight();

private:
    static void task_entry(void *context);
    void run();

    bool take_snapshot(DisplaySnapshot &snapshot);
    void render(const DisplaySnapshot &snapshot);
    void render_idle(const DisplaySnapshot &snapshot);
    void render_therapy(const DisplaySnapshot &snapshot);

    void draw_centered(int16_t y,
                       const char *text,
                       uint16_t color,
                       uint8_t preferred_size);
    void draw_status_row(int16_t y,
                         int16_t height,
                         const char *label,
                         const char *value,
                         uint16_t value_color);
    uint8_t text_size_for_width(const char *text,
                                uint8_t preferred_size,
                                int16_t max_width) const;

    DisplayDevice *device_ = nullptr;
    TaskHandle_t task_ = nullptr;
    StaticSemaphore_t snapshot_lock_storage_ = {};
    SemaphoreHandle_t snapshot_lock_ = nullptr;
    DisplaySnapshot pending_snapshot_;
    uint32_t published_generation_ = 0;
    uint32_t rendered_generation_ = 0;
    std::atomic<bool> backlight_requested_{true};
    bool backlight_applied_ = false;
};

}  // namespace aircannect
