#include "can_driver.h"

#include <string.h>

#include "debug_log.h"

namespace aircannect {

namespace {

const char *esp_err_name_short(esp_err_t err) {
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        default: return "ESP_ERR";
    }
}

twai_timing_config_t can_timing_config() {
#if AC_CAN_TIMING == AC_CAN_TIMING_20TQ_SP80
#if AC_CAN_BITRATE != 1000000
#error "AC_CAN_TIMING_20TQ_SP80 supports AC_CAN_BITRATE=1000000 only."
#endif
    return {TWAI_CLK_SRC_DEFAULT, 20000000, 0, 0, 15, 4, 3, 0, false};
#elif AC_CAN_TIMING == AC_CAN_TIMING_STOCK
#if AC_CAN_BITRATE == 1000000
    return TWAI_TIMING_CONFIG_1MBITS();
#elif AC_CAN_BITRATE == 800000
    return TWAI_TIMING_CONFIG_800KBITS();
#elif AC_CAN_BITRATE == 500000
    return TWAI_TIMING_CONFIG_500KBITS();
#elif AC_CAN_BITRATE == 250000
    return TWAI_TIMING_CONFIG_250KBITS();
#elif AC_CAN_BITRATE == 125000
    return TWAI_TIMING_CONFIG_125KBITS();
#else
#error "Unsupported AC_CAN_BITRATE. Add a TWAI timing preset in can_timing_config()."
#endif
#else
#error "Unsupported AC_CAN_TIMING."
#endif
}

const char *can_timing_name() {
#if AC_CAN_TIMING == AC_CAN_TIMING_20TQ_SP80
    return "20tq_sp80";
#elif AC_CAN_TIMING == AC_CAN_TIMING_STOCK
    return "stock";
#else
    return "unknown";
#endif
}

}  // namespace

bool CanDriver::begin() {
    twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(
        static_cast<gpio_num_t>(AC_CAN_TX_GPIO),
        static_cast<gpio_num_t>(AC_CAN_RX_GPIO),
        TWAI_MODE_NORMAL);
    general_config.tx_queue_len = 16;
    general_config.rx_queue_len = AC_CAN_RX_QUEUE_LEN;

    twai_timing_config_t timing_config = can_timing_config();
    twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&general_config, &timing_config,
                                        &filter_config);
    if (err != ESP_OK) {
        Log::logf(CAT_CAN, LOG_ERROR, "[CAN] driver install failed: %s\n",
                  esp_err_name_short(err));
        return false;
    }
    installed_ = true;

    if (!start_controller()) return false;

    Log::logf(CAT_CAN, LOG_INFO,
              "[CAN] started bitrate=%d timing=%s tx_gpio=%d rx_gpio=%d tx_id=0x%03X rx_id=0x%03X\n",
              AC_CAN_BITRATE, can_timing_name(), AC_CAN_TX_GPIO,
              AC_CAN_RX_GPIO, AC_CAN_TX_ID, AC_CAN_RX_ID);
    return true;
}

bool CanDriver::start_controller() {
    esp_err_t err = twai_start();
    if (err != ESP_OK) {
        Log::logf(CAT_CAN, LOG_ERROR, "[CAN] start failed: %s\n",
                  esp_err_name_short(err));
        return false;
    }

    const uint32_t alerts = TWAI_ALERT_TX_SUCCESS |
                            TWAI_ALERT_TX_FAILED |
                            TWAI_ALERT_RX_QUEUE_FULL |
                            TWAI_ALERT_ERR_PASS |
                            TWAI_ALERT_ERR_ACTIVE |
                            TWAI_ALERT_BUS_ERROR |
                            TWAI_ALERT_BUS_OFF |
                            TWAI_ALERT_ARB_LOST |
                            TWAI_ALERT_ABOVE_ERR_WARN |
                            TWAI_ALERT_BELOW_ERR_WARN |
                            TWAI_ALERT_BUS_RECOVERED;
    err = twai_reconfigure_alerts(alerts, nullptr);
    if (err != ESP_OK) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] alert config failed: %s\n",
                  esp_err_name_short(err));
    }
    return true;
}

void CanDriver::poll() {
    if (!installed_) return;

    uint32_t alerts = 0;
    esp_err_t err = twai_read_alerts(&alerts, 0);
    if (err == ESP_OK && alerts) {
        handle_alerts(alerts);
    } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] alert read failed: %s\n",
                  esp_err_name_short(err));
    }

    if (recovery_active_) {
        poll_recovery(alerts);
        return;
    }

    pump_tx_queue(alerts);
}

bool CanDriver::enqueue_tx(const RawCanFrame &frame) {
    if (frame.len > 8) return false;
    if (recovery_active_) return false;
    if (!tx_queue_.push(frame)) {
        stats_.tx_queue_drops++;
        return false;
    }
    return true;
}

void CanDriver::pump_tx_queue(uint32_t alerts) {
    const uint32_t now = millis();
    if (alerts & TWAI_ALERT_TX_SUCCESS) {
        last_tx_success_ms_ = now;
    }

    if (alerts & TWAI_ALERT_BUS_OFF) {
        stats_.tx_failures++;
        recover_or_restart("CAN bus off");
        return;
    }

    if (alerts & TWAI_ALERT_TX_FAILED) {
        stats_.tx_failures++;
        recover_or_restart("CAN TX failed");
        return;
    }

    twai_status_info_t status = {};
    if (twai_get_status_info(&status) != ESP_OK ||
        status.state != TWAI_STATE_RUNNING) {
        return;
    }

    const bool busy_before = status.msgs_to_tx > 0 || tx_queue_.count() > 0;
    if (busy_before && !last_tx_success_ms_) last_tx_success_ms_ = now;
    if (busy_before &&
        static_cast<int32_t>(now - last_tx_success_ms_) >= 100) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] TX confirmation timeout\n");
        stats_.tx_failures++;
        recover_or_restart("CAN TX timeout");
        return;
    }

    for (size_t pumped = 0; pumped < AC_CAN_TX_DRAIN_BUDGET; ++pumped) {
        RawCanFrame frame;
        if (!tx_queue_.pop(frame)) break;

        twai_message_t msg = {};
        msg.identifier = frame.id;
        msg.extd = frame.extended ? 1 : 0;
        msg.rtr = frame.remote ? 1 : 0;
        msg.data_length_code = frame.len;
        memcpy(msg.data, frame.data, frame.len);

        esp_err_t err = twai_transmit(&msg, 0);
        if (err == ESP_ERR_TIMEOUT) {
            tx_queue_.push_front(frame);
            break;
        }
        if (err != ESP_OK) {
            Log::logf(CAT_CAN, LOG_WARN, "[CAN] transmit failed: %s\n",
                      esp_err_name_short(err));
            stats_.tx_failures++;
            recover_or_restart("CAN transmit failed");
            return;
        }
        stats_.tx_frames++;
        if (!last_tx_success_ms_) last_tx_success_ms_ = now;
    }

    if (!tx_queue_.count()) {
        twai_status_info_t after = {};
        if (twai_get_status_info(&after) == ESP_OK && after.msgs_to_tx == 0) {
            last_tx_success_ms_ = 0;
        }
    }
}

bool CanDriver::receive(RawCanFrame &frame, uint32_t wait_ms) {
    if (!installed_) return false;

    twai_message_t msg = {};
    esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(wait_ms));
    if (err == ESP_ERR_TIMEOUT) return false;
    if (err != ESP_OK) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] RX failed: %s\n",
                  esp_err_name_short(err));
        return false;
    }

    frame = {};
    frame.id = msg.identifier;
    frame.len = msg.data_length_code;
    frame.extended = msg.extd != 0;
    frame.remote = msg.rtr != 0;
    memcpy(frame.data, msg.data, frame.len);
    stats_.rx_frames++;
    return true;
}

void CanDriver::handle_alerts(uint32_t alerts) {
    stats_.alerts_seen++;
    if (alerts & TWAI_ALERT_BUS_ERROR) stats_.bus_error_alerts++;
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) stats_.rx_queue_full_alerts++;

    const uint32_t visible_alerts = alerts & ~TWAI_ALERT_TX_SUCCESS;
    if (!visible_alerts) return;

    if (visible_alerts == TWAI_ALERT_BUS_ERROR &&
        static_cast<int32_t>(millis() - last_bus_error_log_ms_) < 1000) {
        suppressed_bus_error_alerts_++;
        return;
    }

    const uint32_t suppressed = suppressed_bus_error_alerts_;
    suppressed_bus_error_alerts_ = 0;
    if (suppressed) {
        Log::logf(CAT_CAN, LOG_INFO,
                  "[CAN] alert:%s%s%s%s%s%s%s%s%s%s "
                  "suppressed_bus_errors=%lu\n",
                  (visible_alerts & TWAI_ALERT_TX_FAILED) ? " tx_failed" : "",
                  (visible_alerts & TWAI_ALERT_RX_QUEUE_FULL) ? " rx_queue_full" : "",
                  (visible_alerts & TWAI_ALERT_ERR_PASS) ? " err_passive" : "",
                  (visible_alerts & TWAI_ALERT_ERR_ACTIVE) ? " err_active" : "",
                  (visible_alerts & TWAI_ALERT_BUS_ERROR) ? " bus_error" : "",
                  (visible_alerts & TWAI_ALERT_BUS_OFF) ? " bus_off" : "",
                  (visible_alerts & TWAI_ALERT_ARB_LOST) ? " arb_lost" : "",
                  (visible_alerts & TWAI_ALERT_ABOVE_ERR_WARN) ? " above_err_warn" : "",
                  (visible_alerts & TWAI_ALERT_BELOW_ERR_WARN) ? " below_err_warn" : "",
                  (visible_alerts & TWAI_ALERT_BUS_RECOVERED) ? " bus_recovered" : "",
                  static_cast<unsigned long>(suppressed));
    } else {
        Log::logf(CAT_CAN, LOG_INFO,
                  "[CAN] alert:%s%s%s%s%s%s%s%s%s%s\n",
                  (visible_alerts & TWAI_ALERT_TX_FAILED) ? " tx_failed" : "",
                  (visible_alerts & TWAI_ALERT_RX_QUEUE_FULL) ? " rx_queue_full" : "",
                  (visible_alerts & TWAI_ALERT_ERR_PASS) ? " err_passive" : "",
                  (visible_alerts & TWAI_ALERT_ERR_ACTIVE) ? " err_active" : "",
                  (visible_alerts & TWAI_ALERT_BUS_ERROR) ? " bus_error" : "",
                  (visible_alerts & TWAI_ALERT_BUS_OFF) ? " bus_off" : "",
                  (visible_alerts & TWAI_ALERT_ARB_LOST) ? " arb_lost" : "",
                  (visible_alerts & TWAI_ALERT_ABOVE_ERR_WARN) ? " above_err_warn" : "",
                  (visible_alerts & TWAI_ALERT_BELOW_ERR_WARN) ? " below_err_warn" : "",
                  (visible_alerts & TWAI_ALERT_BUS_RECOVERED) ? " bus_recovered" : "");
    }
    if (visible_alerts & TWAI_ALERT_BUS_ERROR) last_bus_error_log_ms_ = millis();
}

bool CanDriver::recover_or_restart(const char *reason) {
    if (!installed_) return false;
    if (recovery_active_) return true;

    Log::logf(CAT_CAN, LOG_WARN, "[CAN] recovery requested: %s\n", reason);

    twai_status_info_t status = {};
    esp_err_t err = twai_get_status_info(&status);
    if (err != ESP_OK) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] status before recovery failed: %s\n",
                  esp_err_name_short(err));
        stats_.recovery_failures++;
        return false;
    }

    if (status.state == TWAI_STATE_BUS_OFF) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] bus_off: initiating TWAI recovery\n");
        err = twai_initiate_recovery();
        if (err != ESP_OK) {
            Log::logf(CAT_CAN, LOG_ERROR, "[CAN] initiate recovery failed: %s\n",
                      esp_err_name_short(err));
            stats_.recovery_failures++;
            return false;
        }
        recovery_active_ = true;
        recovery_deadline_ms_ = millis() + 1500;
        recovery_timeout_reported_ = false;
        return true;
    } else if (status.state == TWAI_STATE_RECOVERING) {
        recovery_active_ = true;
        recovery_deadline_ms_ = millis() + 1500;
        recovery_timeout_reported_ = false;
        return true;
    } else if (status.state == TWAI_STATE_RUNNING) {
        err = twai_stop();
        if (err != ESP_OK) {
            Log::logf(CAT_CAN, LOG_ERROR, "[CAN] stop failed: %s\n",
                      esp_err_name_short(err));
            stats_.recovery_failures++;
            return false;
        }
    } else if (status.state != TWAI_STATE_STOPPED) {
        Log::logf(CAT_CAN, LOG_ERROR, "[CAN] cannot recover from state %s\n",
                  state_name(status.state));
        stats_.recovery_failures++;
        return false;
    }

    return restart_after_recovery();
}

bool CanDriver::restart_after_recovery() {
    (void)twai_clear_transmit_queue();
    (void)twai_clear_receive_queue();
    tx_queue_.clear();
    last_tx_success_ms_ = 0;

    esp_err_t err = twai_start();
    if (err != ESP_OK) {
        Log::logf(CAT_CAN, LOG_ERROR, "[CAN] start after recovery failed: %s\n",
                  esp_err_name_short(err));
        stats_.recovery_failures++;
        recovery_active_ = false;
        return false;
    }

    recovery_active_ = false;
    recovery_timeout_reported_ = false;
    stats_.recoveries++;
    Log::logf(CAT_CAN, LOG_INFO, "[CAN] controller restarted\n");
    return true;
}

void CanDriver::poll_recovery(uint32_t alerts) {
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        restart_after_recovery();
        return;
    }

    twai_status_info_t status = {};
    if (twai_get_status_info(&status) == ESP_OK &&
        status.state == TWAI_STATE_STOPPED) {
        restart_after_recovery();
        return;
    }

    if (!recovery_timeout_reported_ &&
        static_cast<int32_t>(millis() - recovery_deadline_ms_) >= 0) {
        Log::logf(CAT_CAN, LOG_WARN, "[CAN] bus-off recovery timed out\n");
        stats_.recovery_failures++;
        recovery_timeout_reported_ = true;
    }
}

void CanDriver::clear_rx_state() {
    if (installed_) (void)twai_clear_receive_queue();
}

void CanDriver::print_status(Print &out) const {
    twai_status_info_t status = {};
    esp_err_t err = twai_get_status_info(&status);
    if (err != ESP_OK) {
        out.print("[CAN] status failed: ");
        out.println(esp_err_name_short(err));
        return;
    }

    out.print("[CAN] state=");
    out.print(state_name(status.state));
    out.print(" tx_err=");
    out.print(status.tx_error_counter);
    out.print(" rx_err=");
    out.print(status.rx_error_counter);
    out.print(" tx_q=");
    out.print(status.msgs_to_tx);
    out.print(" rx_q=");
    out.print(status.msgs_to_rx);
    out.print(" tx_failed=");
    out.print(status.tx_failed_count);
    out.print(" rx_missed=");
    out.print(status.rx_missed_count);
    out.print(" rx_overrun=");
    out.print(status.rx_overrun_count);
    out.print(" arb_lost=");
    out.print(status.arb_lost_count);
    out.print(" bus_errors=");
    out.println(status.bus_error_count);
}

void CanDriver::print_stats(Print &out) const {
    twai_status_info_t status = {};
    const bool have_status = twai_get_status_info(&status) == ESP_OK;

    out.print(" can_rx_frames=");
    out.print(stats_.rx_frames);
    out.print(" can_tx_frames=");
    out.print(stats_.tx_frames);
    out.print(" can_tx_q=");
    out.print(tx_queue_.count());
    out.print(" can_tx_q_drops=");
    out.print(stats_.tx_queue_drops);
    out.print(" can_tx_failures=");
    out.print(stats_.tx_failures);
    out.print(" recoveries=");
    out.print(stats_.recoveries);
    out.print(" recovery_failures=");
    out.print(stats_.recovery_failures);
    out.print(" alerts=");
    out.print(stats_.alerts_seen);
    out.print(" bus_error_alerts=");
    out.print(stats_.bus_error_alerts);
    out.print(" rx_queue_full_alerts=");
    out.print(stats_.rx_queue_full_alerts);
    if (have_status) {
        out.print(" twai_state=");
        out.print(state_name(status.state));
        out.print(" tx_err=");
        out.print(status.tx_error_counter);
        out.print(" rx_err=");
        out.print(status.rx_error_counter);
        out.print(" twai_tx_q=");
        out.print(status.msgs_to_tx);
        out.print(" twai_rx_q=");
        out.print(status.msgs_to_rx);
        out.print(" tx_failed=");
        out.print(status.tx_failed_count);
        out.print(" rx_missed=");
        out.print(status.rx_missed_count);
        out.print(" rx_overrun=");
        out.print(status.rx_overrun_count);
        out.print(" arb_lost=");
        out.print(status.arb_lost_count);
        out.print(" bus_errors=");
        out.print(status.bus_error_count);
    }
}

void CanDriver::reset_stats() {
    stats_ = {};
    tx_queue_.reset_dropped();
}

size_t CanDriver::tx_queue_free() const {
    if (recovery_active_) return 0;
    return tx_queue_.free();
}

size_t CanDriver::tx_queue_depth() const {
    return tx_queue_.count();
}

const char *CanDriver::state_name(twai_state_t state) const {
    switch (state) {
        case TWAI_STATE_STOPPED: return "stopped";
        case TWAI_STATE_RUNNING: return "running";
        case TWAI_STATE_BUS_OFF: return "bus_off";
        case TWAI_STATE_RECOVERING: return "recovering";
        default: return "unknown";
    }
}

}  // namespace aircannect
