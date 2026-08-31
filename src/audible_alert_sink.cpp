#include "audible_alert_sink.h"

#include <esp_heap_caps.h>

#include "debug_log.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

constexpr uint32_t ALERT_REPEAT_MS = 5000;
constexpr uint16_t ALERT_TONE_HZ = 880;
constexpr uint16_t ALERT_TONE_MS = 140;
constexpr uint32_t ALERT_TONE_GAP_MS = 120;
constexpr uint8_t ALERT_TONES_PER_BURST = 3;
constexpr uint32_t AUDIO_TASK_STACK = 3072;
constexpr UBaseType_t AUDIO_TASK_PRIORITY = 1;
constexpr BaseType_t AUDIO_TASK_CORE = 0;
constexpr uint32_t AUDIO_START_RETRY_MS = 10000;

}  // namespace

bool AudibleAlertSink::begin(AudibleOutput &output, uint32_t now_ms) {
    output_ = &output;
    if (task_) return true;
    if (start()) return true;

    next_start_ms_ = now_ms + AUDIO_START_RETRY_MS;
    return false;
}

void AudibleAlertSink::poll(uint32_t now_ms) {
    if (task_ || !output_ ||
        !enabled_.load(std::memory_order_acquire) ||
        static_cast<int32_t>(now_ms - next_start_ms_) < 0) {
        return;
    }

    if (!start()) {
        next_start_ms_ = now_ms + AUDIO_START_RETRY_MS;
    }
}

bool AudibleAlertSink::start() {
    if (!output_ || !output_->begin()) return false;
    if (!output_->set_volume(
            volume_percent_.load(std::memory_order_acquire))) {
        output_->silence();
        return false;
    }

    BaseType_t created = pdFAIL;
    if (Memory::psram_available()) {
        created = xTaskCreatePinnedToCoreWithCaps(
            task_entry, "ac_alert_audio", AUDIO_TASK_STACK, this,
            AUDIO_TASK_PRIORITY, &task_, AUDIO_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS || !task_) {
        task_ = nullptr;
        created = xTaskCreatePinnedToCore(
            task_entry, "ac_alert_audio", AUDIO_TASK_STACK, this,
            AUDIO_TASK_PRIORITY, &task_, AUDIO_TASK_CORE);
    }
    if (created == pdPASS && task_) return true;

    task_ = nullptr;
    output_->silence();
    Log::logf(CAT_GENERAL, LOG_ERROR,
              "[AUDIO] alert task creation failed\n");
    return false;
}

void AudibleAlertSink::set_enabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled && output_) output_->silence();
    if (task_) xTaskNotifyGive(task_);
}

void AudibleAlertSink::set_volume(uint8_t percent) {
    if (percent > 100) return;

    const uint8_t previous = volume_percent_.exchange(
        percent, std::memory_order_acq_rel);
    if (previous != percent && task_) xTaskNotifyGive(task_);
}

void AudibleAlertSink::accept(const AlertEvent &event) {
    const uint8_t kind = static_cast<uint8_t>(event.kind);
    if (kind >= 32) return;

    const uint32_t bit = 1UL << kind;
    if (event.state == AlertState::Raised) {
        active_alerts_.fetch_or(bit, std::memory_order_acq_rel);
    } else {
        active_alerts_.fetch_and(~bit, std::memory_order_acq_rel);
    }
    if (task_) xTaskNotifyGive(task_);
}

void AudibleAlertSink::task_entry(void *context) {
    static_cast<AudibleAlertSink *>(context)->task_loop();
}

void AudibleAlertSink::task_loop() {
    uint8_t applied_volume =
        volume_percent_.load(std::memory_order_acquire);

    while (true) {
        const uint8_t requested_volume =
            volume_percent_.load(std::memory_order_acquire);
        if (requested_volume != applied_volume && output_ &&
            output_->set_volume(requested_volume)) {
            applied_volume = requested_volume;
        }

        if (!active()) {
            if (output_) output_->silence();
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        bool interrupted = false;
        for (uint8_t tone = 0; tone < ALERT_TONES_PER_BURST; ++tone) {
            if (output_) {
                (void)output_->play_tone(ALERT_TONE_HZ, ALERT_TONE_MS);
            }
            if (!active()) break;
            if (tone + 1 >= ALERT_TONES_PER_BURST) continue;

            if (ulTaskNotifyTake(
                    pdTRUE, pdMS_TO_TICKS(ALERT_TONE_GAP_MS)) > 0) {
                interrupted = true;
                break;
            }
        }
        if (!active() || interrupted) continue;

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ALERT_REPEAT_MS));
    }
}

bool AudibleAlertSink::active() const {
    return output_ && enabled_.load(std::memory_order_acquire) &&
           active_alerts_.load(std::memory_order_acquire) != 0;
}

}  // namespace aircannect
