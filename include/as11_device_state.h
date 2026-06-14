#pragma once

#include <stdint.h>
#include <string>

#include "as11_event_frame.h"

namespace aircannect {

const char *as11_identity_get_params_json();
const char *as11_runtime_get_params_json();
const char *as11_motor_runtime_get_params_json();
const char *as11_timezone_get_params_json();
bool as11_parse_event_subscription_response(const std::string &payload,
                                            bool require_activity_selectors,
                                            uint32_t &subscription_id);

enum class As11TherapyState {
    Unknown,
    Standby,
    Running,
    Other,
};

enum class As11TherapyTarget {
    None,
    Standby,
    Running,
};

class As11DeviceState {
public:
    void reset();
    void poll(uint32_t now_ms);

    // Incoming payloads
    bool apply_status_get_response(const std::string &payload,
                                   uint32_t now_ms);
    bool apply_datetime_response(const std::string &payload,
                                 uint32_t now_ms,
                                 int64_t request_epoch_ms = 0,
                                 int64_t response_epoch_ms = 0);
    bool apply_activity_subscription_response(const std::string &payload,
                                              uint32_t now_ms,
                                              uint32_t &subscription_id);
    bool apply_activity_event_frame(const As11EventFrame &frame,
                                    uint32_t now_ms);

    // Locally initiated therapy transitions
    void mark_therapy_command_sent(const std::string &method,
                                   uint32_t now_ms);
    void mark_therapy_command_response(const std::string &method,
                                       bool is_error,
                                       uint32_t now_ms);
    void mark_therapy_command_timeout(const std::string &method,
                                      uint32_t now_ms);
    void clear_pending_therapy_command(const char *reason, uint32_t now_ms);

    // Cache freshness
    bool status_valid() const { return status_valid_; }
    uint32_t status_updated_ms() const { return status_updated_ms_; }
    bool clock_valid() const { return clock_valid_; }
    uint32_t clock_sample_ms() const { return clock_sample_ms_; }
    bool clock_offset_valid() const { return clock_offset_valid_; }
    int32_t clock_offset_ms() const { return clock_offset_ms_; }

    // Identity and runtime facts
    const std::string &serial_number() const { return serial_number_; }
    const std::string &product_name() const { return product_name_; }
    const std::string &software_identifier() const {
        return software_identifier_;
    }
    const std::string &active_therapy_profile() const {
        return active_therapy_profile_;
    }
    const std::string &mhr() const { return mhr_; }
    bool platform_id_valid() const { return platform_id_valid_; }
    int32_t platform_id() const { return platform_id_; }
    bool variant_id_valid() const { return variant_id_valid_; }
    int32_t variant_id() const { return variant_id_; }
    bool timezone_offset_valid() const { return timezone_offset_valid_; }
    int32_t timezone_offset_minutes() const {
        return timezone_offset_minutes_;
    }

    // Activity and therapy state
    const std::string &rop() const { return rop_; }
    const std::string &last_activity_event() const {
        return last_activity_event_;
    }
    const std::string &last_activity_event_report_time() const {
        return last_activity_event_report_time_;
    }
    uint32_t last_activity_event_ms() const {
        return last_activity_event_ms_;
    }
    As11TherapyState therapy_state() const { return therapy_state_; }
    bool therapy_command_pending() const {
        return pending_therapy_target_ != As11TherapyTarget::None;
    }
    As11TherapyTarget pending_therapy_target() const {
        return pending_therapy_target_;
    }
    const std::string &last_therapy_command_status() const {
        return last_therapy_command_status_;
    }
    const std::string &device_datetime() const { return device_datetime_; }

    // helpers
    static bool is_therapy_command_method(const std::string &method);
    static const char *therapy_state_name(As11TherapyState state);
    static const char *therapy_target_name(As11TherapyTarget target);

private:
    void update_rop(const std::string &value, uint32_t now_ms);
    void confirm_pending_if_matched(uint32_t now_ms);

    bool status_valid_ = false;
    uint32_t status_updated_ms_ = 0;

    bool clock_valid_ = false;
    uint32_t clock_sample_ms_ = 0;
    bool clock_offset_valid_ = false;
    int32_t clock_offset_ms_ = 0;

    std::string serial_number_;
    std::string product_name_;
    std::string software_identifier_;
    std::string active_therapy_profile_;
    std::string mhr_;
    bool platform_id_valid_ = false;
    int32_t platform_id_ = 0;
    bool variant_id_valid_ = false;
    int32_t variant_id_ = 0;
    bool timezone_offset_valid_ = false;
    int32_t timezone_offset_minutes_ = 0;

    std::string rop_;
    As11TherapyState therapy_state_ = As11TherapyState::Unknown;
    std::string last_activity_event_;
    std::string last_activity_event_report_time_;
    uint32_t last_activity_event_ms_ = 0;

    std::string device_datetime_;

    As11TherapyTarget pending_therapy_target_ = As11TherapyTarget::None;
    uint32_t pending_therapy_since_ms_ = 0;
    std::string last_therapy_command_status_;
};

}  // namespace aircannect
