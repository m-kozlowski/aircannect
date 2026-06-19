#pragma once

#include <Arduino.h>
#include <driver/twai.h>
#include <stdint.h>

#include "board.h"
#include "fixed_queue.h"

namespace aircannect {

struct RawCanFrame {
    uint32_t id = 0;
    uint8_t len = 0;
    uint8_t data[8] = {};
    bool extended = false;
    bool remote = false;
};

struct CanDriverStats {
    uint32_t rx_frames = 0;
    uint32_t tx_frames = 0;
    uint32_t tx_queue_drops = 0;
    uint32_t tx_failures = 0;
    uint32_t recoveries = 0;
    uint32_t recovery_failures = 0;
    uint32_t bus_error_alerts = 0;
    uint32_t rx_queue_full_alerts = 0;
};

struct CanControllerStatus {
    bool valid = false;
    esp_err_t error = ESP_OK;
    twai_state_t state = TWAI_STATE_STOPPED;
    uint32_t tx_error_counter = 0;
    uint32_t rx_error_counter = 0;
    uint32_t msgs_to_tx = 0;
    uint32_t msgs_to_rx = 0;
    uint32_t tx_failed_count = 0;
    uint32_t rx_missed_count = 0;
    uint32_t rx_overrun_count = 0;
    uint32_t arb_lost_count = 0;
    uint32_t bus_error_count = 0;
};

class CanDriver {
public:
    bool begin();
    void poll();

    bool enqueue_tx(const RawCanFrame &frame);
    bool receive(RawCanFrame &frame, uint32_t wait_ms = 0);

    bool recover_or_restart(const char *reason);
    void clear_rx_state();

    void reset_stats();

    size_t tx_queue_free() const;
    size_t tx_queue_depth() const;
    bool controller_status(CanControllerStatus &out) const;
    const CanDriverStats &stats() const { return stats_; }
    static const char *state_name(twai_state_t state);
    static const char *error_name(esp_err_t err);

private:
    void poll_recovery(uint32_t alerts);
    bool restart_after_recovery();
    bool start_controller();
    void handle_alerts(uint32_t alerts);

    void pump_tx_queue(uint32_t alerts);

    FixedQueue<RawCanFrame, AC_CAN_TX_QUEUE_DEPTH> tx_queue_;
    uint32_t last_tx_success_ms_ = 0;

    uint32_t last_bus_error_log_ms_ = 0;
    uint32_t suppressed_bus_error_alerts_ = 0;
    uint32_t last_rx_queue_full_missed_count_ = 0;

    uint32_t recovery_deadline_ms_ = 0;
    bool recovery_active_ = false;
    bool recovery_timeout_reported_ = false;

    bool installed_ = false;
    CanDriverStats stats_ = {};
};

}  // namespace aircannect
