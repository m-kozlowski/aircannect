#include "display_manager.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "board_display.h"
#include "board_motion.h"
#include "debug_log.h"
#include "display_device.h"
#include "memory_manager.h"
#include "motion_device.h"

namespace aircannect {
namespace {

constexpr uint16_t COLOR_BACKGROUND = 0x0841;
constexpr uint16_t COLOR_PANEL = 0x10C3;
constexpr uint16_t COLOR_TEXT = 0xF7BE;
constexpr uint16_t COLOR_MUTED = 0x8410;
constexpr uint16_t COLOR_ACCENT = 0x06FA;
constexpr uint16_t COLOR_READY = 0x27E8;
constexpr uint16_t COLOR_PENDING = 0xFD20;
constexpr uint16_t COLOR_ERROR = 0xF986;
constexpr uint32_t MANUAL_OFF_WAKE_GRACE_MS = 3000;

const char *air11_state_text(DisplayAir11State state) {
    switch (state) {
        case DisplayAir11State::Ready: return "READY";
        case DisplayAir11State::Unavailable: return "UNAVAILABLE";
        case DisplayAir11State::Connecting: return "CONNECTING";
    }
    return "--";
}

uint16_t air11_state_color(DisplayAir11State state) {
    switch (state) {
        case DisplayAir11State::Ready: return COLOR_READY;
        case DisplayAir11State::Unavailable: return COLOR_ERROR;
        case DisplayAir11State::Connecting: return COLOR_PENDING;
    }
    return COLOR_MUTED;
}

const char *export_state_text(DisplayExportState state) {
    switch (state) {
        case DisplayExportState::Disabled: return "OFF";
        case DisplayExportState::Ready: return "READY";
        case DisplayExportState::Pending: return "PENDING";
        case DisplayExportState::Working: return "SYNCING";
        case DisplayExportState::Error: return "ERROR";
    }
    return "--";
}

uint16_t export_state_color(DisplayExportState state) {
    switch (state) {
        case DisplayExportState::Ready: return COLOR_READY;
        case DisplayExportState::Pending:
        case DisplayExportState::Working: return COLOR_PENDING;
        case DisplayExportState::Error: return COLOR_ERROR;
        case DisplayExportState::Disabled: return COLOR_MUTED;
    }
    return COLOR_MUTED;
}

}  // namespace

bool DisplayManager::begin() {
    device_ = board_display_device();
    if (!device_) return true;

    if (!device_->begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[DISPLAY] initialization failed\n");
        return false;
    }

    motion_ = board_motion_device();
    if (motion_ && !motion_->begin()) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[DISPLAY] motion sensor unavailable\n");
        motion_ = nullptr;
    }

    snapshot_lock_ = xSemaphoreCreateMutexStatic(&snapshot_lock_storage_);
    if (!snapshot_lock_) return false;

    BaseType_t created = pdFAIL;
    if (Memory::psram_available()) {
        created = xTaskCreatePinnedToCoreWithCaps(
            task_entry, "ac_display", AC_DISPLAY_TASK_STACK, this,
            AC_DISPLAY_TASK_PRIO, &task_, AC_DISPLAY_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (created != pdPASS || !task_) {
        task_ = nullptr;
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[DISPLAY] PSRAM task creation failed\n");
        return false;
    }

    return true;
}

void DisplayManager::publish(const DisplaySnapshot &snapshot) {
    if (!device_ || !snapshot_lock_) return;
    if (xSemaphoreTake(snapshot_lock_, 0) != pdTRUE) return;

    pending_snapshot_ = snapshot;
    ++published_generation_;
    if (published_generation_ == 0) ++published_generation_;
    pending_snapshot_.generation = published_generation_;
    xSemaphoreGive(snapshot_lock_);

    if (task_) xTaskNotifyGive(task_);
}

void DisplayManager::publish_pressure(
    const DisplayPressureSnapshot &pressure) {
    if (!device_ || !snapshot_lock_) return;
    if (xSemaphoreTake(snapshot_lock_, 0) != pdTRUE) return;
    if (published_generation_ == 0) {
        xSemaphoreGive(snapshot_lock_);
        return;
    }

    pending_snapshot_.pressure = pressure;
    ++published_generation_;
    if (published_generation_ == 0) ++published_generation_;
    pending_snapshot_.generation = published_generation_;
    xSemaphoreGive(snapshot_lock_);

    if (task_) xTaskNotifyGive(task_);
}

void DisplayManager::toggle_backlight() {
    if (!device_) return;

    const bool visible = backlight_visible_.load();
    backlight_requested_.store(!visible);
    if (visible) manual_backlight_off_.store(true);
    if (task_) xTaskNotifyGive(task_);
}

void DisplayManager::task_entry(void *context) {
    static_cast<DisplayManager *>(context)->run();
}

void DisplayManager::run() {
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[DISPLAY] started size=%dx%d framebuffer=psram motion=%s\n",
              static_cast<int>(device_->width()),
              static_cast<int>(device_->height()),
              motion_ ? "yes" : "no");

    uint32_t last_motion_poll_ms = 0;
    while (true) {
        const uint32_t poll_ms = motion_ ? AC_MOTION_POLL_MS : 1000;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_ms));

        const uint32_t now_ms = millis();
        bool rotation_changed = false;

        if (motion_ &&
            (last_motion_poll_ms == 0 ||
             now_ms - last_motion_poll_ms >= AC_MOTION_POLL_MS)) {
            last_motion_poll_ms = now_ms;
            rotation_changed = poll_motion(now_ms);
        }

        if (manual_backlight_off_.exchange(false)) {
            motion_wake_until_ms_ = 0;
            motion_wake_blocked_until_ms_ =
                now_ms + MANUAL_OFF_WAKE_GRACE_MS;
        }

        const bool visible = backlight_requested_.load() ||
                             temporary_wake_active(now_ms);
        if (visible != backlight_applied_) {
            device_->set_backlight(visible);
            backlight_applied_ = visible;
            backlight_visible_.store(visible);
        }

        DisplaySnapshot snapshot;
        if (!take_snapshot(snapshot, rotation_changed)) continue;

        render(snapshot);
        rendered_generation_ = snapshot.generation;
    }
}

bool DisplayManager::take_snapshot(DisplaySnapshot &snapshot, bool force) {
    if (!snapshot_lock_) return false;
    if (xSemaphoreTake(snapshot_lock_, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }

    if (published_generation_ == 0) {
        xSemaphoreGive(snapshot_lock_);
        return false;
    }

    if (!force && published_generation_ == rendered_generation_) {
        xSemaphoreGive(snapshot_lock_);
        return false;
    }

    snapshot = pending_snapshot_;
    xSemaphoreGive(snapshot_lock_);
    return force || snapshot.generation != rendered_generation_;
}

bool DisplayManager::poll_motion(uint32_t now_ms) {
    if (!motion_) return false;

    MotionSample sample;
    if (!motion_->read(sample)) return false;

    const DisplayMotionUpdate update = motion_policy_.update(sample, now_ms);
    if (update.wake && !motion_wake_blocked(now_ms)) {
        motion_wake_until_ms_ = now_ms + AC_MOTION_WAKE_MS;
    }
    if (!update.rotation_changed) return false;

    device_->set_rotation(update.rotation);
    return true;
}

bool DisplayManager::temporary_wake_active(uint32_t now_ms) const {
    return motion_wake_until_ms_ != 0 &&
           static_cast<int32_t>(motion_wake_until_ms_ - now_ms) > 0;
}

bool DisplayManager::motion_wake_blocked(uint32_t now_ms) const {
    return motion_wake_blocked_until_ms_ != 0 &&
           static_cast<int32_t>(motion_wake_blocked_until_ms_ - now_ms) > 0;
}

void DisplayManager::render(const DisplaySnapshot &snapshot) {
    device_->fill(COLOR_BACKGROUND);

    if (snapshot.therapy_active) render_therapy(snapshot);
    else render_idle(snapshot);

    device_->flush();
}

void DisplayManager::render_idle(const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;
    const int16_t header_height = height / 4;
    const int16_t row_height =
        (height - header_height - margin) / 4;

    draw_centered(margin, snapshot.local_time,
                  COLOR_TEXT, width >= 220 ? 5 : 4);

    int16_t y = header_height;
    draw_status_row(y, row_height, "AIR11",
                    air11_state_text(snapshot.air11),
                    air11_state_color(snapshot.air11));
    y += row_height;

    draw_status_row(y, row_height,
                    snapshot.wifi == DisplayWifiState::SoftAp
                        ? "WI-FI AP" : "WI-FI",
                    snapshot.wifi_value[0] ? snapshot.wifi_value : "--",
                    snapshot.wifi == DisplayWifiState::Offline
                        ? COLOR_ERROR : COLOR_TEXT);
    y += row_height;

    draw_status_row(y, row_height, "SMB",
                    export_state_text(snapshot.smb),
                    export_state_color(snapshot.smb));
    y += row_height;

    draw_status_row(y, row_height, "SLEEPHQ",
                    export_state_text(snapshot.sleephq),
                    export_state_color(snapshot.sleephq));
}

void DisplayManager::render_therapy(const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;

    device_->draw_text(margin, margin, snapshot.local_time,
                       COLOR_TEXT, 2);

    const char *therapy_label = "THERAPY";
    const int16_t therapy_label_width =
        static_cast<int16_t>(strlen(therapy_label) * 6);
    device_->draw_text(width - margin - therapy_label_width,
                       margin + 4, therapy_label, COLOR_ACCENT, 1);

    char elapsed[12] = {};
    const uint32_t hours = snapshot.therapy_elapsed_s / 3600;
    const uint32_t minutes = (snapshot.therapy_elapsed_s / 60) % 60;
    snprintf(elapsed, sizeof(elapsed), "%02u:%02u",
             static_cast<unsigned>(hours),
             static_cast<unsigned>(minutes));
    draw_centered(height / 6, elapsed, COLOR_TEXT,
                  width >= 220 ? 4 : 3);

    const bool pressure_pair =
        snapshot.pressure.kind == DisplayPressureSnapshot::Kind::Pair;

    device_->draw_text(margin, height * 2 / 5,
                       pressure_pair ? "PRESSURES" : "PRESSURE",
                       COLOR_MUTED, 1);

    if (snapshot.pressure.kind ==
        DisplayPressureSnapshot::Kind::Unavailable) {
        draw_centered(height / 2, "--", COLOR_MUTED, 4);
    } else if (pressure_pair) {
        const int16_t column_width = (width - margin * 3) / 2;
        char ipap[12] = {};
        char epap[12] = {};
        snprintf(ipap, sizeof(ipap), "%.1f",
                 snapshot.pressure.inspiratory);
        snprintf(epap, sizeof(epap), "%.1f",
                 snapshot.pressure.expiratory);

        device_->fill_rect(margin, height / 2 - 8,
                           column_width, height / 5, COLOR_PANEL);
        device_->fill_rect(margin * 2 + column_width, height / 2 - 8,
                           column_width, height / 5, COLOR_PANEL);
        device_->draw_text(margin + 8, height / 2 - 2,
                           "IPAP", COLOR_MUTED, 1);
        device_->draw_text(margin + 8, height / 2 + 14,
                           ipap, COLOR_TEXT, 3);
        device_->draw_text(margin * 2 + column_width + 8,
                           height / 2 - 2, "EPAP", COLOR_MUTED, 1);
        device_->draw_text(margin * 2 + column_width + 8,
                           height / 2 + 14, epap, COLOR_TEXT, 3);
    } else {
        char pressure[12] = {};
        snprintf(pressure, sizeof(pressure), "%.1f",
                 snapshot.pressure.inspiratory);
        draw_centered(height / 2 - 4, pressure, COLOR_TEXT, 4);
        draw_centered(height * 7 / 10, "cmH2O", COLOR_MUTED, 1);
    }

    if (!snapshot.oximetry_valid) return;

    char spo2[12] = {};
    char pulse[12] = {};
    snprintf(spo2, sizeof(spo2), "SpO2 %d%%", snapshot.spo2);
    snprintf(pulse, sizeof(pulse), "Pulse %d", snapshot.pulse_bpm);

    const int16_t footer_y = height - margin - 20;
    device_->draw_text(margin, footer_y, spo2, COLOR_READY, 2);
    const uint8_t pulse_size = text_size_for_width(
        pulse, 2, width / 2 - margin);
    const int16_t pulse_width =
        static_cast<int16_t>(strlen(pulse) * 6 * pulse_size);
    device_->draw_text(width - margin - pulse_width, footer_y,
                       pulse, COLOR_TEXT, pulse_size);
}

void DisplayManager::draw_centered(int16_t y,
                                   const char *text,
                                   uint16_t color,
                                   uint8_t preferred_size) {
    const int16_t width = device_->width();
    const uint8_t size = text_size_for_width(
        text, preferred_size, width - width / 10);
    const int16_t text_width =
        static_cast<int16_t>(strlen(text) * 6 * size);
    device_->draw_text((width - text_width) / 2, y, text, color, size);
}

void DisplayManager::draw_status_row(int16_t y,
                                     int16_t height,
                                     const char *label,
                                     const char *value,
                                     uint16_t value_color) {
    const int16_t width = device_->width();
    const int16_t margin = width / 20;
    device_->fill_rect(margin, y + 2,
                       width - margin * 2, height - 4,
                       COLOR_PANEL);
    device_->draw_text(margin + 8, y + 7, label, COLOR_MUTED, 1);

    const uint8_t size = text_size_for_width(
        value, 2, width - margin * 2 - 16);
    device_->draw_text(margin + 8, y + 20, value, value_color, size);
}

uint8_t DisplayManager::text_size_for_width(const char *text,
                                            uint8_t preferred_size,
                                            int16_t max_width) const {
    const size_t length = text ? strlen(text) : 0;
    uint8_t size = preferred_size > 0 ? preferred_size : 1;
    while (size > 1 && length * 6 * size >
                           static_cast<size_t>(max_width)) {
        --size;
    }
    return size;
}

}  // namespace aircannect
