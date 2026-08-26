#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <stdint.h>

#include "display_config.h"
#include "display_motion.h"
#include "display_snapshot.h"

namespace aircannect {

class DisplayDevice;
class MotionDevice;

class DisplayManager {
public:
    void configure(DisplayOrientation orientation, bool auto_rotate);
    bool begin();
    void publish(const DisplaySnapshot &snapshot);
    void publish_therapy_telemetry(
        const TherapyTelemetrySnapshot &telemetry,
        int therapy_mode);

    bool available() const { return device_ != nullptr; }
    bool backlight_on() const { return backlight_visible_.load(); }
    void toggle_backlight();
    bool previous_page();
    bool next_page();

private:
    static void task_entry(void *context);
    void run();

    bool take_snapshot(DisplaySnapshot &snapshot, bool force);
    bool poll_motion(uint32_t now_ms);
    void apply_display_config();
    bool navigate_page(int8_t direction);
    bool temporary_wake_active(uint32_t now_ms) const;
    bool motion_wake_blocked(uint32_t now_ms) const;
    void render(const DisplaySnapshot &snapshot);
    void render_idle(const DisplaySnapshot &snapshot);
    void render_idle_dashboard(const DisplaySnapshot &snapshot);
    void render_idle_latest(const DisplaySnapshot &snapshot);
    void render_idle_period(const DisplaySnapshot &snapshot);
    void render_therapy(const DisplaySnapshot &snapshot);
    void render_therapy_primary(const DisplaySnapshot &snapshot);
    void render_therapy_detail(const DisplaySnapshot &snapshot);
    void draw_page_indicator(uint8_t page, uint8_t count);

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
    MotionDevice *motion_ = nullptr;
    TaskHandle_t task_ = nullptr;
    StaticSemaphore_t snapshot_lock_storage_ = {};
    SemaphoreHandle_t snapshot_lock_ = nullptr;
    DisplaySnapshot pending_snapshot_;
    uint32_t published_generation_ = 0;
    uint32_t rendered_generation_ = 0;

    DisplayMotionPolicy motion_policy_;
    std::atomic<uint8_t> configured_rotation_{0};
    std::atomic<bool> auto_rotate_{true};
    std::atomic<bool> config_dirty_{false};
    uint32_t motion_wake_until_ms_ = 0;
    uint32_t motion_wake_blocked_until_ms_ = 0;
    std::atomic<bool> backlight_requested_{true};
    std::atomic<bool> backlight_visible_{false};
    std::atomic<bool> manual_backlight_off_{false};
    std::atomic<bool> navigation_wake_requested_{false};
    std::atomic<bool> page_dirty_{false};
    std::atomic<bool> therapy_active_{false};
    std::atomic<uint8_t> idle_page_{0};
    std::atomic<uint8_t> therapy_page_{0};
    std::atomic<uint8_t> therapy_page_count_{1};
    bool backlight_applied_ = false;
};

}  // namespace aircannect
