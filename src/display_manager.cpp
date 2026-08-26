#include "display_manager.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "board_display.h"
#include "board_motion.h"
#include "debug_log.h"
#include "display_device.h"
#include "display_page_navigation.h"
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
constexpr uint8_t IDLE_PAGE_COUNT = 3;
constexpr uint8_t THERAPY_PAGE_COUNT = 2;

struct TherapyDetailRow {
    const char *label = nullptr;
    char value[20] = {};
};

void format_duration(uint32_t minutes, char *out, size_t size) {
    const uint32_t hours = minutes / 60;
    const uint32_t remainder = minutes % 60;
    snprintf(out, size, "%uh %02um",
             static_cast<unsigned>(hours),
             static_cast<unsigned>(remainder));
}

void format_ie_ratio(float percent, char *out, size_t size) {
    if (percent <= 0.0f) {
        snprintf(out, size, "--");
    } else if (percent < 100.0f) {
        snprintf(out, size, "1:%.1f", 100.0f / percent);
    } else {
        snprintf(out, size, "%.1f:1", percent / 100.0f);
    }
}

void format_metric_pair(bool first_valid,
                        float first,
                        bool second_valid,
                        float second,
                        char *out,
                        size_t size) {
    if (first_valid && second_valid) {
        snprintf(out, size, "%.1f / %.1f", first, second);
    } else if (first_valid) {
        snprintf(out, size, "%.1f / --", first);
    } else {
        snprintf(out, size, "-- / --");
    }
}

size_t therapy_detail_rows(const DisplaySnapshot &snapshot,
                           TherapyDetailRow *rows,
                           size_t capacity) {
    size_t count = 0;
    auto append = [&](const char *label, const char *format, float value) {
        if (count >= capacity) return;
        rows[count].label = label;
        snprintf(rows[count].value, sizeof(rows[count].value),
                 format, value);
        ++count;
    };

    if (snapshot.tidal_volume_valid) {
        append("TIDAL VOLUME", "%.0f mL", snapshot.tidal_volume_ml);
    }
    if (snapshot.minute_ventilation_valid) {
        append("MINUTE VENT.", "%.1f L/min",
               snapshot.minute_ventilation_l_min);
    }
    if (snapshot.flow_limitation_valid) {
        append("FLOW LIMIT", "%.2f", snapshot.flow_limitation);
    }
    if (snapshot.inspiratory_duration_valid) {
        append("INSP. TIME", "%.2f s", snapshot.inspiratory_duration_s);
    }
    if (snapshot.ie_ratio_valid && count < capacity) {
        rows[count].label = "I:E";
        format_ie_ratio(snapshot.ie_ratio_percent,
                        rows[count].value,
                        sizeof(rows[count].value));
        ++count;
    }
    if (snapshot.snore_valid) {
        append("SNORE", "%.2f", snapshot.snore);
    }
    return count;
}

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

void DisplayManager::configure(DisplayOrientation orientation,
                               bool auto_rotate) {
    configured_rotation_.store(display_orientation_rotation(
        orientation, AC_DISPLAY_ROTATION));
    auto_rotate_.store(auto_rotate);
    config_dirty_.store(true);
    if (task_) xTaskNotifyGive(task_);
}

bool DisplayManager::begin() {
    device_ = board_display_device();
    if (!device_) return true;

    if (!device_->begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[DISPLAY] initialization failed\n");
        return false;
    }

    device_->set_rotation(configured_rotation_.load());
    motion_policy_.set_rotation(configured_rotation_.load());
    config_dirty_.store(false);

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

    const uint8_t therapy_page_count = snapshot.therapy_active
        ? THERAPY_PAGE_COUNT : 1;
    therapy_page_count_.store(therapy_page_count);
    if (therapy_page_.load() >= therapy_page_count) {
        therapy_page_.store(0);
        page_dirty_.store(true);
    }

    if (published_generation_ != 0 &&
        pending_snapshot_.therapy_active != snapshot.therapy_active) {
        if (snapshot.therapy_active) therapy_page_.store(0);
        else idle_page_.store(0);
        page_dirty_.store(true);
    }

    pending_snapshot_ = snapshot;
    therapy_active_.store(snapshot.therapy_active);
    ++published_generation_;
    if (published_generation_ == 0) ++published_generation_;
    pending_snapshot_.generation = published_generation_;
    xSemaphoreGive(snapshot_lock_);

    if (task_) xTaskNotifyGive(task_);
}

void DisplayManager::publish_therapy_telemetry(
    const TherapyTelemetrySnapshot &telemetry,
    int therapy_mode) {
    if (!device_ || !snapshot_lock_) return;
    if (xSemaphoreTake(snapshot_lock_, 0) != pdTRUE) return;
    if (published_generation_ == 0) {
        xSemaphoreGive(snapshot_lock_);
        return;
    }

    apply_display_therapy_telemetry(
        pending_snapshot_, telemetry, therapy_mode);
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

bool DisplayManager::previous_page() {
    return navigate_page(-1);
}

bool DisplayManager::next_page() {
    return navigate_page(1);
}

bool DisplayManager::navigate_page(int8_t direction) {
    if (!device_ || direction == 0) return false;

    const bool therapy = therapy_active_.load();
    std::atomic<uint8_t> &page = therapy ? therapy_page_ : idle_page_;
    const uint8_t count = therapy
        ? therapy_page_count_.load() : IDLE_PAGE_COUNT;
    const DisplayPageNavigation navigation = display_page_navigate(
        page.load(), count, direction, backlight_visible_.load());
    if (!navigation.handled) return false;

    if (navigation.wake_only) {
        navigation_wake_requested_.store(true);
        if (task_) xTaskNotifyGive(task_);
        return true;
    }

    page.store(navigation.page);
    page_dirty_.store(navigation.changed);
    if (task_) xTaskNotifyGive(task_);
    return true;
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

        if (config_dirty_.exchange(false)) {
            apply_display_config();
            rotation_changed = true;
        }

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
        if (navigation_wake_requested_.exchange(false)) {
            motion_wake_until_ms_ = now_ms + AC_MOTION_WAKE_MS;
        }

        const bool visible = backlight_requested_.load() ||
                             temporary_wake_active(now_ms);
        if (visible != backlight_applied_) {
            device_->set_backlight(visible);
            backlight_applied_ = visible;
            backlight_visible_.store(visible);
        }

        DisplaySnapshot snapshot;
        const bool page_changed = page_dirty_.exchange(false);
        if (!take_snapshot(snapshot, rotation_changed || page_changed)) {
            continue;
        }

        render(snapshot);
        rendered_generation_ = snapshot.generation;
    }
}

void DisplayManager::apply_display_config() {
    if (!device_) return;

    const uint8_t rotation = configured_rotation_.load();
    motion_policy_.set_rotation(rotation);
    device_->set_rotation(rotation);
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
    if (!auto_rotate_.load() || !update.rotation_changed) return false;

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
    const uint8_t page = idle_page_.load() % IDLE_PAGE_COUNT;
    if (page == 1) render_idle_latest(snapshot);
    else if (page == 2) render_idle_period(snapshot);
    else render_idle_dashboard(snapshot);

    draw_page_indicator(page, IDLE_PAGE_COUNT);
}

void DisplayManager::render_idle_dashboard(
    const DisplaySnapshot &snapshot) {
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

void DisplayManager::render_idle_latest(const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;
    const int16_t header_height = height / 5;
    const int16_t row_height = (height - header_height - margin) / 4;
    const DisplayNightSummary &night = snapshot.reports.latest;

    draw_centered(margin, night.valid ? night.date : "LATEST NIGHT",
                  COLOR_ACCENT, 2);

    char value[24] = {};
    int16_t y = header_height;
    if (night.duration_valid) {
        format_duration(night.duration_min, value, sizeof(value));
    } else {
        snprintf(value, sizeof(value), "--");
    }
    draw_status_row(y, row_height, "THERAPY", value, COLOR_TEXT);
    y += row_height;

    format_metric_pair(night.ahi_valid, night.ahi,
                       night.rdi_valid, night.rdi,
                       value, sizeof(value));
    draw_status_row(y, row_height, "AHI / RDI", value, COLOR_TEXT);
    y += row_height;

    format_metric_pair(night.pressure_valid, night.pressure_cm_h2o,
                       night.pressure_95_valid,
                       night.pressure_95_cm_h2o,
                       value, sizeof(value));
    draw_status_row(y, row_height, "PRESSURE P50 / P95", value, COLOR_TEXT);
    y += row_height;

    format_metric_pair(night.leak_valid, night.leak_l_min,
                       night.leak_95_valid, night.leak_95_l_min,
                       value, sizeof(value));
    draw_status_row(y, row_height, "LEAK P50 / P95", value, COLOR_TEXT);
}

void DisplayManager::render_idle_period(const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;
    const int16_t header_height = height / 5;
    const int16_t row_height = (height - header_height - margin) / 4;
    const DisplayPeriodSummary &period = snapshot.reports.last_30_days;

    draw_centered(margin, "LAST 30 DAYS", COLOR_ACCENT, 2);

    char value[24] = {};
    int16_t y = header_height;
    if (period.valid) {
        snprintf(value, sizeof(value), "%u",
                 static_cast<unsigned>(period.used_nights));
    } else {
        snprintf(value, sizeof(value), "--");
    }
    draw_status_row(y, row_height, "USED NIGHTS", value, COLOR_TEXT);
    y += row_height;

    format_metric_pair(period.ahi_valid,
                       period.duration_weighted_ahi,
                       period.rdi_valid,
                       period.duration_weighted_rdi,
                       value, sizeof(value));
    draw_status_row(y, row_height, "AHI / RDI", value, COLOR_TEXT);
    y += row_height;

    format_metric_pair(
        period.pressure_valid,
        period.duration_weighted_pressure_cm_h2o,
        period.pressure_95_valid,
        period.duration_weighted_pressure_95_cm_h2o,
        value, sizeof(value));
    draw_status_row(y, row_height, "PRESSURE P50 / P95", value, COLOR_TEXT);
    y += row_height;

    format_metric_pair(period.leak_valid,
                       period.duration_weighted_leak_l_min,
                       period.leak_95_valid,
                       period.duration_weighted_leak_95_l_min,
                       value, sizeof(value));
    draw_status_row(y, row_height, "LEAK P50 / P95", value, COLOR_TEXT);
}

void DisplayManager::render_therapy(const DisplaySnapshot &snapshot) {
    const uint8_t count = therapy_page_count_.load();
    const uint8_t page = therapy_page_.load() % count;
    if (page == 1) render_therapy_detail(snapshot);
    else render_therapy_primary(snapshot);

    draw_page_indicator(page, count);
}

void DisplayManager::render_therapy_primary(
    const DisplaySnapshot &snapshot) {
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
    const int16_t pressure_label_y = height / 3;
    const int16_t pressure_value_y = height * 2 / 5;

    device_->draw_text(margin, pressure_label_y,
                       pressure_pair ? "PRESSURES" : "PRESSURE",
                       COLOR_MUTED, 1);

    if (snapshot.pressure.kind ==
        DisplayPressureSnapshot::Kind::Unavailable) {
        draw_centered(pressure_value_y, "--", COLOR_MUTED, 4);
    } else if (pressure_pair) {
        const int16_t column_width = (width - margin * 3) / 2;
        const int16_t pressure_panel_y = pressure_value_y - 4;
        const int16_t pressure_panel_height = height / 5;
        char ipap[12] = {};
        char epap[12] = {};
        snprintf(ipap, sizeof(ipap), "%.1f",
                 snapshot.pressure.inspiratory);
        snprintf(epap, sizeof(epap), "%.1f",
                 snapshot.pressure.expiratory);

        device_->fill_rect(margin, pressure_panel_y,
                           column_width, pressure_panel_height,
                           COLOR_PANEL);
        device_->fill_rect(margin * 2 + column_width,
                           pressure_panel_y,
                           column_width, pressure_panel_height,
                           COLOR_PANEL);
        device_->draw_text(margin + 8, pressure_panel_y + 6,
                           "IPAP", COLOR_MUTED, 1);
        device_->draw_text(margin + 8, pressure_panel_y + 22,
                           ipap, COLOR_TEXT, 3);
        device_->draw_text(margin * 2 + column_width + 8,
                           pressure_panel_y + 6,
                           "EPAP", COLOR_MUTED, 1);
        device_->draw_text(margin * 2 + column_width + 8,
                           pressure_panel_y + 22,
                           epap, COLOR_TEXT, 3);
    } else {
        char pressure[12] = {};
        snprintf(pressure, sizeof(pressure), "%.1f",
                 snapshot.pressure.inspiratory);
        draw_centered(pressure_value_y, pressure, COLOR_TEXT, 4);
    }

    const int16_t metric_gap = margin / 2;
    const int16_t metric_width =
        (width - margin * 2 - metric_gap) / 2;
    const int16_t metric_right_x = margin + metric_width + metric_gap;
    const int16_t metric_top = height * 3 / 5;
    const int16_t metric_row_gap = 4;
    const int16_t metric_bottom = height - margin - 8;
    const int16_t metric_height =
        (metric_bottom - metric_top - metric_row_gap) / 2;
    auto draw_metric = [&](int16_t x,
                           int16_t y,
                           const char *label,
                           const char *value) {
        device_->fill_rect(x, y, metric_width, metric_height,
                           COLOR_PANEL);
        device_->draw_text(x + 7, y + 5, label, COLOR_MUTED, 1);
        device_->draw_text(x + 7, y + 17, value, COLOR_TEXT, 2);
    };

    char leak[12] = "--";
    char rate[12] = "--";
    if (snapshot.leak_valid) {
        snprintf(leak, sizeof(leak), "%.1f", snapshot.leak_l_min);
    }
    if (snapshot.respiratory_rate_valid) {
        snprintf(rate, sizeof(rate), "%.1f",
                 snapshot.respiratory_rate);
    }
    draw_metric(margin, metric_top, "LEAK", leak);
    draw_metric(metric_right_x, metric_top, "RR", rate);

    if (!snapshot.oximetry_valid) return;

    const int16_t oximetry_y =
        metric_top + metric_height + metric_row_gap;
    char spo2[12] = {};
    char pulse[12] = {};
    snprintf(spo2, sizeof(spo2), "%d%%", snapshot.spo2);
    snprintf(pulse, sizeof(pulse), "%d", snapshot.pulse_bpm);
    draw_metric(margin, oximetry_y, "SPO2", spo2);
    draw_metric(metric_right_x, oximetry_y, "PULSE", pulse);
}

void DisplayManager::render_therapy_detail(
    const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;
    const int16_t header_height = height / 4;

    if (snapshot.pressure.kind == DisplayPressureSnapshot::Kind::Pair) {
        char pressure[24] = {};
        device_->draw_text(margin, margin, "IPAP / EPAP",
                           COLOR_MUTED, 1);
        snprintf(pressure, sizeof(pressure), "%.1f / %.1f",
                 snapshot.pressure.inspiratory,
                 snapshot.pressure.expiratory);
        draw_centered(margin + 14, pressure, COLOR_ACCENT, 2);
    } else if (snapshot.pressure.kind ==
               DisplayPressureSnapshot::Kind::Single) {
        char pressure[20] = {};
        device_->draw_text(margin, margin, "PRESSURE", COLOR_MUTED, 1);
        snprintf(pressure, sizeof(pressure), "%.1f",
                 snapshot.pressure.inspiratory);
        draw_centered(margin + 14, pressure, COLOR_ACCENT, 2);
    } else {
        device_->draw_text(margin, margin, "PRESSURE", COLOR_MUTED, 1);
        draw_centered(margin + 14, "--", COLOR_MUTED, 2);
    }

    TherapyDetailRow rows[4];
    const size_t row_count = therapy_detail_rows(
        snapshot, rows, sizeof(rows) / sizeof(rows[0]));
    const int16_t row_height =
        (height - header_height - margin) /
        static_cast<int16_t>(sizeof(rows) / sizeof(rows[0]));
    int16_t y = header_height;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (i < row_count) {
            draw_status_row(y, row_height, rows[i].label,
                            rows[i].value, COLOR_TEXT);
        } else {
            draw_status_row(y, row_height, "", "--", COLOR_MUTED);
        }
        y += row_height;
    }
}

void DisplayManager::draw_page_indicator(uint8_t page, uint8_t count) {
    if (count < 2) return;

    char text[8] = {};
    snprintf(text, sizeof(text), "%u/%u",
             static_cast<unsigned>(page + 1),
             static_cast<unsigned>(count));
    const int16_t width = device_->width();
    const int16_t text_width = static_cast<int16_t>(strlen(text) * 6);
    device_->draw_text(width - width / 20 - text_width,
                       device_->height() - 10,
                       text, COLOR_MUTED, 1);
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
