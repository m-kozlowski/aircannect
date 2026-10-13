#include "display_manager.h"

#include <Arduino.h>
#include <assert.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "board_display.h"
#include "board_motion.h"
#include "debug_log.h"
#include "display_device.h"
#include "display_page_navigation.h"
#include "large_object.h"
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
constexpr uint8_t COMPACT_THERAPY_PAGE_COUNT = 4;

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

struct DisplayManager::Field {
    struct Text {
        int16_t x = 0;
        int16_t y = 0;
        uint16_t color = 0;
        uint8_t size = 1;
        char value[40] = {};

        void set(int16_t left, int16_t top, const char *text,
                 uint16_t text_color, uint8_t text_size,
                 size_t max_chars = sizeof(value) - 1) {
            x = left;
            y = top;
            color = text_color;
            size = text_size;
            const size_t limit = max_chars < sizeof(value)
                ? max_chars : sizeof(value) - 1;
            snprintf(value, limit + 1, "%s", text);
            if (limit >= 3 && strlen(text) > limit) {
                memcpy(value + limit - 3, "...", 3);
            }
        }

        bool equals(const Text &other) const {
            return x == other.x && y == other.y &&
                   color == other.color && size == other.size &&
                   strcmp(value, other.value) == 0;
        }
    };

    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t background = COLOR_BACKGROUND;
    Text text[2];

    bool same_layout(const Field &other) const {
        return x == other.x && y == other.y &&
               width == other.width && height == other.height &&
               background == other.background;
    }

    bool same_content(const Field &other) const {
        return text[0].equals(other.text[0]) &&
               text[1].equals(other.text[1]);
    }
};

struct DisplayManager::RenderState {
    static constexpr size_t MAX_FIELDS = 16;
    Field fields[MAX_FIELDS];
    Field rendered[MAX_FIELDS];
    size_t count = 0;
    size_t rendered_count = 0;
    bool therapy = false;
    uint8_t page = 0;
};

struct DisplayManager::StatusRow {
    const char *label = nullptr;
    char value[40] = {};
    uint16_t color = COLOR_TEXT;
};

void DisplayManager::configure(DisplayOrientation orientation,
                               bool auto_rotate, uint16_t timeout_s) {
    configured_rotation_.store(display_orientation_rotation(
        orientation, AC_DISPLAY_ROTATION));
    auto_rotate_.store(auto_rotate);
    idle_timeout_ms_.store(static_cast<uint32_t>(timeout_s) * 1000);
    config_dirty_.store(true);
    if (task_) xTaskNotifyGive(task_);
}

bool DisplayManager::begin() {
    device_ = board_display_device();
    if (!device_) return true;

    compact_layout_ = device_->width() < 200 || device_->height() < 200;

    if (!device_->begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[DISPLAY] initialization failed\n");
        return false;
    }

    device_->set_rotation(configured_rotation_.load());
    motion_policy_.set_rotation(configured_rotation_.load());
    config_dirty_.store(false);
    last_interaction_ms_ = millis();

    motion_ = board_motion_device();
    if (motion_ && !motion_->begin()) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[DISPLAY] motion sensor unavailable\n");
        motion_ = nullptr;
    }

    snapshot_lock_ = xSemaphoreCreateMutexStatic(&snapshot_lock_storage_);
    if (!snapshot_lock_) return false;

    render_state_ = LargeObject::create<RenderState>();
    if (!render_state_) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[DISPLAY] field buffer allocation failed\n");
        return false;
    }

    BaseType_t created = pdFAIL;
    if (Memory::psram_available()) {
        created = xTaskCreatePinnedToCoreWithCaps(
            task_entry, "ac_display", AC_DISPLAY_TASK_STACK, this,
            AC_DISPLAY_TASK_PRIO, &task_, AC_DISPLAY_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (created != pdPASS || !task_) {
        task_ = nullptr;
        LargeObject::destroy(render_state_);
        render_state_ = nullptr;
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[DISPLAY] PSRAM task creation failed\n");
        return false;
    }

    return true;
}

bool DisplayManager::snapshot_due(uint32_t now_ms) const {
    return available() &&
           (!snapshot_published_ || now_ms - last_snapshot_ms_ >= 1000);
}

void DisplayManager::publish(const DisplaySnapshot &snapshot, uint32_t now_ms) {
    if (!device_ || !snapshot_lock_) return;
    if (xSemaphoreTake(snapshot_lock_, 0) != pdTRUE) return;

    const uint8_t therapy_page_count = snapshot.therapy_active
        ? (compact_layout_ ? COMPACT_THERAPY_PAGE_COUNT : THERAPY_PAGE_COUNT)
        : 1;
    therapy_page_count_.store(therapy_page_count);
    if (therapy_page_.load() >= therapy_page_count) {
        therapy_page_.store(0);
        presentation_dirty_.store(true);
    }

    if (published_generation_ != 0 &&
        pending_snapshot_.therapy_active != snapshot.therapy_active) {
        if (snapshot.therapy_active) therapy_page_.store(0);
        else idle_page_.store(0);
        presentation_dirty_.store(true);
    }

    pending_snapshot_ = snapshot;
    therapy_active_.store(snapshot.therapy_active);
    ++published_generation_;
    if (published_generation_ == 0) ++published_generation_;
    pending_snapshot_.generation = published_generation_;
    xSemaphoreGive(snapshot_lock_);

    last_snapshot_ms_ = now_ms;
    snapshot_published_ = true;
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

void DisplayManager::set_backlight(bool enabled) {
    if (!device_) return;

    backlight_request_.store(enabled ? BacklightRequest::On
                                    : BacklightRequest::Off);
    if (task_) xTaskNotifyGive(task_);
}

void DisplayManager::toggle_backlight() {
    set_backlight(!backlight_visible_.load());
}

void DisplayManager::note_interaction() {
    interaction_pending_.store(true);
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
        ? therapy_page_count_.load() : idle_page_count();
    const DisplayPageNavigation navigation = display_page_navigate(
        page.load(), count, direction, backlight_visible_.load());
    if (!navigation.handled) return false;

    note_interaction();
    if (navigation.wake_only) {
        set_backlight(true);
        return true;
    }

    page.store(navigation.page);
    if (navigation.changed) presentation_dirty_.store(true);
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

        if (config_dirty_.exchange(false)) {
            apply_display_config();
            presentation_dirty_.store(true);
            last_interaction_ms_ = now_ms;
        }

        if (interaction_pending_.exchange(false)) {
            last_interaction_ms_ = now_ms;
        }

        const BacklightRequest request =
            backlight_request_.exchange(BacklightRequest::None);

        if (request != BacklightRequest::None) {
            awake_ = request == BacklightRequest::On;
            last_interaction_ms_ = now_ms;
            if (!awake_) {
                motion_wake_blocked_until_ms_ =
                    now_ms + MANUAL_OFF_WAKE_GRACE_MS;
            }
        }

        if (motion_ &&
            (last_motion_poll_ms == 0 ||
             now_ms - last_motion_poll_ms >= AC_MOTION_POLL_MS)) {
            last_motion_poll_ms = now_ms;
            if (poll_motion(now_ms)) presentation_dirty_.store(true);
        }

        const uint32_t timeout_ms = idle_timeout_ms_.load();
        if (timeout_ms != 0 && now_ms - last_interaction_ms_ >= timeout_ms) {
            awake_ = false;
        }

        const bool visible = awake_;
        const bool waking = visible && !backlight_applied_;
        if (!visible) {
            if (backlight_applied_) {
                device_->set_backlight(false);
                backlight_applied_ = false;
                backlight_visible_.store(false);
            }
            continue;
        }

        DisplaySnapshot snapshot;
        const bool presentation_dirty = presentation_dirty_.exchange(false);
        const bool force = waking || presentation_dirty;
        const bool frame_pending = take_snapshot(snapshot, force);
        if (!frame_pending) {
            if (presentation_dirty) presentation_dirty_.store(true);
            continue;
        }

        render(snapshot, force);
        rendered_generation_ = snapshot.generation;

        if (waking) {
            device_->set_backlight(true);
            backlight_applied_ = true;
            backlight_visible_.store(true);
        }
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
        last_interaction_ms_ = now_ms;
        awake_ = true;
    }
    if (!auto_rotate_.load() || !update.rotation_changed) return false;

    device_->set_rotation(update.rotation);
    return true;
}

bool DisplayManager::motion_wake_blocked(uint32_t now_ms) const {
    return motion_wake_blocked_until_ms_ != 0 &&
           static_cast<int32_t>(motion_wake_blocked_until_ms_ - now_ms) > 0;
}

void DisplayManager::render(const DisplaySnapshot &snapshot, bool force) {
    RenderState &state = *render_state_;
    state.count = 0;
    const uint8_t count = snapshot.therapy_active
        ? therapy_page_count_.load() : idle_page_count();
    const uint8_t page = (snapshot.therapy_active
        ? therapy_page_.load() : idle_page_.load()) % count;

    if (snapshot.therapy_active) render_therapy(snapshot, page, count);
    else render_idle(snapshot, page);

    flush_fields(force || state.therapy != snapshot.therapy_active ||
                 state.page != page);
    state.therapy = snapshot.therapy_active;
    state.page = page;
}

void DisplayManager::render_idle(const DisplaySnapshot &snapshot,
                                 uint8_t page) {
    const uint8_t section = compact_layout_ ? page / 2 : page;
    const uint8_t part = compact_layout_ ? page % 2 : 0;
    if (section == 1) render_idle_latest(snapshot, part);
    else if (section == 2) render_idle_period(snapshot, part);
    else render_idle_dashboard(snapshot, part);

    draw_page_indicator(page, idle_page_count());
}

uint8_t DisplayManager::idle_page_count() const {
    return compact_layout_ ? IDLE_PAGE_COUNT * 2 : IDLE_PAGE_COUNT;
}

void DisplayManager::render_idle_dashboard(
    const DisplaySnapshot &snapshot, uint8_t part) {
    StatusRow rows[4] = {
        {"AIR11", {}, air11_state_color(snapshot.air11)},
        {snapshot.wifi == DisplayWifiState::SoftAp ? "WI-FI AP" : "WI-FI",
         {}, snapshot.wifi == DisplayWifiState::Offline ? COLOR_ERROR : COLOR_TEXT},
        {"SMB", {}, export_state_color(snapshot.smb)},
        {"SLEEPHQ", {}, export_state_color(snapshot.sleephq)},
    };
    snprintf(rows[0].value, sizeof(rows[0].value), "%s",
             air11_state_text(snapshot.air11));
    snprintf(rows[1].value, sizeof(rows[1].value), "%s",
             snapshot.wifi_value[0] ? snapshot.wifi_value : "--");
    snprintf(rows[2].value, sizeof(rows[2].value), "%s",
             export_state_text(snapshot.smb));
    snprintf(rows[3].value, sizeof(rows[3].value), "%s",
             export_state_text(snapshot.sleephq));

    draw_status_page(snapshot.local_time, COLOR_TEXT,
                     device_->width() >= 220 ? 5 : 4, rows, part);
}

void DisplayManager::render_idle_latest(const DisplaySnapshot &snapshot,
                                       uint8_t part) {
    const DisplayNightSummary &night = snapshot.reports.latest;
    StatusRow rows[4] = {
        {"THERAPY"}, {"AHI / RDI"},
        {compact_layout_ ? "PRESSURE P50/P95" : "PRESSURE P50 / P95"},
        {"LEAK P50 / P95"},
    };
    if (night.duration_valid) {
        format_duration(night.duration_min, rows[0].value, sizeof(rows[0].value));
    } else {
        snprintf(rows[0].value, sizeof(rows[0].value), "--");
    }

    format_metric_pair(night.ahi_valid, night.ahi,
                       night.rdi_valid, night.rdi,
                       rows[1].value, sizeof(rows[1].value));

    format_metric_pair(night.pressure_valid, night.pressure_cm_h2o,
                       night.pressure_95_valid,
                       night.pressure_95_cm_h2o,
                       rows[2].value, sizeof(rows[2].value));

    format_metric_pair(night.leak_valid, night.leak_l_min,
                       night.leak_95_valid, night.leak_95_l_min,
                       rows[3].value, sizeof(rows[3].value));

    draw_status_page(night.valid ? night.date : "LATEST NIGHT",
                     COLOR_ACCENT, 2, rows, part);
}

void DisplayManager::render_idle_period(const DisplaySnapshot &snapshot,
                                       uint8_t part) {
    const DisplayPeriodSummary &period = snapshot.reports.last_30_days;
    StatusRow rows[4] = {
        {"USED NIGHTS"}, {"AHI / RDI"},
        {compact_layout_ ? "PRESSURE P50/P95" : "PRESSURE P50 / P95"},
        {"LEAK P50 / P95"},
    };
    if (period.valid) {
        snprintf(rows[0].value, sizeof(rows[0].value), "%u",
                 static_cast<unsigned>(period.used_nights));
    } else {
        snprintf(rows[0].value, sizeof(rows[0].value), "--");
    }

    format_metric_pair(period.ahi_valid,
                       period.duration_weighted_ahi,
                       period.rdi_valid,
                       period.duration_weighted_rdi,
                       rows[1].value, sizeof(rows[1].value));

    format_metric_pair(
        period.pressure_valid,
        period.duration_weighted_pressure_cm_h2o,
        period.pressure_95_valid,
        period.duration_weighted_pressure_95_cm_h2o,
        rows[2].value, sizeof(rows[2].value));

    format_metric_pair(period.leak_valid,
                       period.duration_weighted_leak_l_min,
                       period.leak_95_valid,
                       period.duration_weighted_leak_95_l_min,
                       rows[3].value, sizeof(rows[3].value));

    draw_status_page("LAST 30 DAYS", COLOR_ACCENT, 2, rows, part);
}

void DisplayManager::draw_status_page(const char *title, uint16_t title_color,
                                      uint8_t title_size, const StatusRow *rows,
                                      uint8_t part) {
    const int16_t height = device_->height();
    const int16_t margin = device_->width() / 20;
    const int16_t header_height = compact_layout_
        ? 32 : height / (title_size >= 4 ? 4 : 5);
    const uint8_t row_count = compact_layout_ ? 2 : 4;
    const int16_t footer = compact_layout_ ? 12 : margin;
    const int16_t row_height = (height - header_height - footer) / row_count;

    draw_centered(compact_layout_ ? 6 : margin, title, title_color,
                  compact_layout_ ? 2 : title_size);

    for (uint8_t i = 0; i < row_count; ++i) {
        const StatusRow &row = rows[part * row_count + i];
        draw_status_row(header_height + i * row_height, row_height,
                        row.label, row.value, row.color);
    }
}

void DisplayManager::render_therapy(const DisplaySnapshot &snapshot,
                                    uint8_t page, uint8_t count) {
    if (compact_layout_ && page == 1) render_therapy_metrics(snapshot);
    else if (page > 0) {
        render_therapy_detail(snapshot, compact_layout_ ? page - 2 : 0);
    } else render_therapy_primary(snapshot);

    draw_page_indicator(page, count);
}

void DisplayManager::render_therapy_primary(
    const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;

    const int16_t top = compact_layout_ ? 6 : margin;
    const bool narrow = width < 200;
    const bool short_screen = height < 200;
    draw_text(margin, top, 60, snapshot.local_time, COLOR_TEXT, 2);

    const char *therapy_label = "THERAPY";
    const int16_t therapy_label_width =
        static_cast<int16_t>(strlen(therapy_label) * 6);
    draw_text(width - margin - therapy_label_width, top + 4,
              therapy_label_width, therapy_label, COLOR_ACCENT, 1);

    char elapsed[12] = {};
    const uint32_t hours = snapshot.therapy_elapsed_s / 3600;
    const uint32_t minutes = (snapshot.therapy_elapsed_s / 60) % 60;
    snprintf(elapsed, sizeof(elapsed), "%02u:%02u",
             static_cast<unsigned>(hours),
             static_cast<unsigned>(minutes));
    draw_centered(short_screen ? 30 : height / 6, elapsed, COLOR_TEXT,
                  compact_layout_ ? 3 : (width >= 220 ? 4 : 3));

    const bool pressure_pair =
        snapshot.pressure.kind == DisplayPressureSnapshot::Kind::Pair;
    const int16_t pressure_label_y = short_screen ? 60 : height / 3;
    const int16_t pressure_value_y = short_screen ? 76 : height * 2 / 5;

    draw_text(margin, pressure_label_y, width - margin * 2,
              pressure_pair ? "PRESSURES" : "PRESSURE", COLOR_MUTED, 1);

    if (snapshot.pressure.kind ==
        DisplayPressureSnapshot::Kind::Unavailable) {
        draw_centered(pressure_value_y, "--", COLOR_MUTED, 4);
    } else if (pressure_pair) {
        const int16_t column_width = narrow
            ? width - margin * 2 : (width - margin * 3) / 2;
        const int16_t pressure_panel_y = pressure_value_y - 4;
        const int16_t pressure_panel_height = narrow
            ? (height - pressure_panel_y - 16 - margin) / 2
            : (compact_layout_ ? 42 : height / 5);
        char ipap[12] = {};
        char epap[12] = {};
        snprintf(ipap, sizeof(ipap), "%.1f",
                 snapshot.pressure.inspiratory);
        snprintf(epap, sizeof(epap), "%.1f",
                 snapshot.pressure.expiratory);

        draw_panel(margin, pressure_panel_y,
                   column_width, pressure_panel_height,
                   "IPAP", ipap, COLOR_TEXT, 3,
                   compact_layout_ ? 6 : 8, 6, compact_layout_ ? 16 : 22);
        draw_panel(narrow ? margin : margin * 2 + column_width,
                   narrow ? pressure_panel_y + pressure_panel_height + margin
                          : pressure_panel_y,
                   column_width, pressure_panel_height,
                   "EPAP", epap, COLOR_TEXT, 3,
                   compact_layout_ ? 6 : 8, 6, compact_layout_ ? 16 : 22);
    } else {
        char pressure[12] = {};
        snprintf(pressure, sizeof(pressure), "%.1f",
                 snapshot.pressure.inspiratory);
        draw_centered(pressure_value_y, pressure, COLOR_TEXT, 4);
    }

    if (!compact_layout_) render_therapy_metrics(snapshot);
}

void DisplayManager::render_therapy_metrics(const DisplaySnapshot &snapshot) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;
    if (compact_layout_) draw_centered(6, "THERAPY", COLOR_ACCENT, 2);

    const int16_t columns = width < 200 ? 1 : 2;
    const int16_t rows = 4 / columns;
    const int16_t metric_gap = margin / 2;
    const int16_t metric_width =
        (width - margin * 2 - metric_gap * (columns - 1)) / columns;
    const int16_t metric_top = compact_layout_ ? 32 : height * 3 / 5;
    const int16_t metric_row_gap = 4;
    const int16_t metric_bottom = height - margin - 8;
    const int16_t metric_height =
        (metric_bottom - metric_top - metric_row_gap * (rows - 1)) / rows;
    auto draw_metric = [&](uint8_t index,
                           const char *label,
                           const char *value) {
        draw_panel(margin + (index % columns) * (metric_width + metric_gap),
                   metric_top + (index / columns) * (metric_height + metric_row_gap),
                   metric_width, metric_height,
                   label, value, COLOR_TEXT, 2, 7, 5, 17);
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
    draw_metric(0, "LEAK", leak);
    draw_metric(1, "RR", rate);

    if (!snapshot.oximetry_valid && !compact_layout_) return;

    char spo2[12] = "--";
    char pulse[12] = "--";
    if (snapshot.oximetry_valid) {
        snprintf(spo2, sizeof(spo2), "%d%%", snapshot.spo2);
        snprintf(pulse, sizeof(pulse), "%d", snapshot.pulse_bpm);
    }
    draw_metric(2, "SPO2", spo2);
    draw_metric(3, "PULSE", pulse);
}

void DisplayManager::render_therapy_detail(
    const DisplaySnapshot &snapshot, uint8_t part) {
    const int16_t width = device_->width();
    const int16_t height = device_->height();
    const int16_t margin = width / 20;
    const int16_t header_height = compact_layout_ ? 24 : height / 4;

    if (compact_layout_) {
        draw_centered(6, "RESPIRATORY", COLOR_ACCENT, 2);
    } else if (snapshot.pressure.kind == DisplayPressureSnapshot::Kind::Pair) {
        char pressure[24] = {};
        draw_text(margin, margin, width - margin * 2,
                  "IPAP / EPAP", COLOR_MUTED, 1);
        snprintf(pressure, sizeof(pressure), "%.1f / %.1f",
                 snapshot.pressure.inspiratory,
                 snapshot.pressure.expiratory);
        draw_centered(margin + 14, pressure, COLOR_ACCENT, 2);
    } else if (snapshot.pressure.kind ==
               DisplayPressureSnapshot::Kind::Single) {
        char pressure[20] = {};
        draw_text(margin, margin, width - margin * 2,
                  "PRESSURE", COLOR_MUTED, 1);
        snprintf(pressure, sizeof(pressure), "%.1f",
                 snapshot.pressure.inspiratory);
        draw_centered(margin + 14, pressure, COLOR_ACCENT, 2);
    } else {
        draw_text(margin, margin, width - margin * 2,
                  "PRESSURE", COLOR_MUTED, 1);
        draw_centered(margin + 14, "--", COLOR_MUTED, 2);
    }

    TherapyDetailRow rows[6];
    const size_t row_count = therapy_detail_rows(
        snapshot, rows, compact_layout_ ? 6 : 4);
    const size_t visible_rows = compact_layout_ ? 3 : 4;
    const int16_t footer = compact_layout_ ? 12 : margin;
    const int16_t row_height =
        (height - header_height - footer) / static_cast<int16_t>(visible_rows);
    int16_t y = header_height;
    for (size_t i = 0; i < visible_rows; ++i) {
        const size_t index = part * visible_rows + i;
        if (index < row_count) {
            draw_status_row(y, row_height, rows[index].label,
                            rows[index].value, COLOR_TEXT);
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
    draw_text(width - width / 20 - text_width, device_->height() - 10,
              text_width, text, COLOR_MUTED, 1);
}

DisplayManager::Field &DisplayManager::next_field() {
    RenderState &state = *render_state_;
    assert(state.count < RenderState::MAX_FIELDS);
    Field &field = state.fields[state.count++];
    field = {};
    return field;
}

void DisplayManager::flush_fields(bool full) {
    RenderState &state = *render_state_;
    // Fixed, non-overlapping fields can be repainted independently.
    // A layout change must also erase the areas occupied by the old fields.
    full = full || state.count != state.rendered_count;
    for (size_t i = 0; !full && i < state.count; ++i) {
        full = !state.fields[i].same_layout(state.rendered[i]);
    }

    if (full) device_->fill(COLOR_BACKGROUND);

    bool changed[RenderState::MAX_FIELDS] = {};
    for (size_t i = 0; i < state.count; ++i) {
        const Field &field = state.fields[i];
        changed[i] = full || !field.same_content(state.rendered[i]);
        if (!changed[i]) continue;

        if (!full || field.background != COLOR_BACKGROUND) {
            device_->fill_rect(field.x, field.y, field.width,
                               field.height, field.background);
        }

        for (const Field::Text &text : field.text) {
            if (!text.value[0]) continue;

            device_->draw_text(field.x + text.x, field.y + text.y,
                               text.value, text.color, text.size);
        }
        state.rendered[i] = field;
    }
    state.rendered_count = state.count;

    if (full) {
        device_->flush();
        return;
    }

    for (size_t i = 0; i < state.count; ++i) {
        if (!changed[i]) continue;

        const Field &field = state.fields[i];
        device_->flush_rect(field.x, field.y, field.width, field.height);
    }
}

void DisplayManager::draw_text(int16_t x, int16_t y, int16_t width,
                               const char *text, uint16_t color,
                               uint8_t size, bool centered) {
    Field &field = next_field();
    field.x = x;
    field.y = y;
    field.width = width;
    field.height = size * 8;

    const int16_t text_width = static_cast<int16_t>(strlen(text) * 6 * size);
    field.text[0].set(centered ? (width - text_width) / 2 : 0,
                      0, text, color, size);
}

void DisplayManager::draw_panel(int16_t x, int16_t y,
                                int16_t width, int16_t height,
                                const char *label, const char *value,
                                uint16_t value_color, uint8_t value_size,
                                int16_t inset, int16_t label_y,
                                int16_t value_y) {
    Field &field = next_field();
    field.x = x;
    field.y = y;
    field.width = width;
    field.height = height;
    field.background = COLOR_PANEL;
    const int16_t text_width = width - inset * 2;
    value_size = text_size_for_width(value, value_size, text_width);
    while (value_size > 1 && value_y + value_size * 8 > height) --value_size;
    field.text[0].set(inset, label_y, label, COLOR_MUTED, 1, text_width / 6);
    field.text[1].set(inset, value_y, value, value_color, value_size,
                      text_width / (6 * value_size));
}

void DisplayManager::draw_centered(int16_t y,
                                   const char *text,
                                   uint16_t color,
                                   uint8_t preferred_size) {
    const int16_t width = device_->width();
    const uint8_t size = text_size_for_width(
        text, preferred_size, width - width / 10);
    draw_text(0, y, width, text, color, size, true);
}

void DisplayManager::draw_status_row(int16_t y,
                                     int16_t height,
                                     const char *label,
                                     const char *value,
                                     uint16_t value_color) {
    const int16_t width = device_->width();
    const int16_t margin = width / 20;
    const uint8_t size = text_size_for_width(
        value, 2, width - margin * 2 - 16);
    const bool short_row = height < 36;
    draw_panel(margin, y + 2, width - margin * 2, height - 4,
               label, value, value_color, size,
               8, short_row ? 3 : 5, short_row ? 13 : 18);
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
