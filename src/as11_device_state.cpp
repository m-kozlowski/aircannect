#include "as11_device_state.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "board.h"
#include "json_util.h"
#include "resmed_device_protocol.h"
#include "string_util.h"
#include "utc_time.h"

namespace aircannect {
namespace {

static constexpr time_t VALID_TIME_MIN_EPOCH = 1609459200;

bool get_string(JsonObjectConst object, const char *name, std::string &out) {
    return name && json_variant_to_string(object[name], out);
}

bool variant_to_int(JsonVariantConst value, int32_t &out) {
    if (value.isNull()) return false;
    if (value.is<int>()) {
        out = value.as<int>();
        return true;
    }
    if (value.is<long>()) {
        out = static_cast<int32_t>(value.as<long>());
        return true;
    }
    if (value.is<const char *>()) {
        char *end = nullptr;
        long parsed = strtol(value.as<const char *>(), &end, 10);
        if (end && *end == 0) {
            out = static_cast<int32_t>(parsed);
            return true;
        }
    }
    return false;
}

bool parse_timezone_offset_text(const char *text, int32_t &out) {
    if (!text || !*text) return false;

    int sign = 1;
    if (*text == '+') {
        text++;
    } else if (*text == '-') {
        sign = -1;
        text++;
    } else {
        return false;
    }

    if (text[0] < '0' || text[0] > '9' ||
        text[1] < '0' || text[1] > '9' ||
        text[2] != ':' ||
        text[3] < '0' || text[3] > '9' ||
        text[4] < '0' || text[4] > '9' ||
        text[5] != 0) {
        return false;
    }

    const int hours = (text[0] - '0') * 10 + (text[1] - '0');
    const int minutes = (text[3] - '0') * 10 + (text[4] - '0');
    if (hours > 24 || minutes > 59 || (hours == 24 && minutes != 0)) {
        return false;
    }

    out = static_cast<int32_t>(sign * (hours * 60 + minutes));
    return true;
}

bool parse_timezone_offset_minutes(JsonVariantConst value, int32_t &out) {
    int32_t parsed = 0;
    if (variant_to_int(value, parsed)) {
        if (parsed < -24 * 60 || parsed > 24 * 60) return false;
        out = parsed;
        return true;
    }

    if (value.is<const char *>()) {
        return parse_timezone_offset_text(value.as<const char *>(), out);
    }
    return false;
}

As11TherapyState classify_rop(const std::string &value) {
    const std::string normalized = lower_compact_copy(value);
    if (normalized.empty()) return As11TherapyState::Unknown;
    if (normalized == "standby" || normalized == "0" ||
        normalized == "0000") {
        return As11TherapyState::Standby;
    }
    if (normalized == "normal" || normalized == "therapy" ||
        normalized == "running" || normalized == "1" ||
        normalized == "0001") {
        return As11TherapyState::Running;
    }
    return As11TherapyState::Other;
}

As11TherapyTarget target_for_method(const std::string &method) {
    if (method == "EnterTherapy") return As11TherapyTarget::Running;
    if (method == "EnterStandby") return As11TherapyTarget::Standby;
    return As11TherapyTarget::None;
}

As11TherapyState therapy_state_for_event(const std::string &event) {
    if (event == "TherapyStarted" || event == "TherapyStart") {
        return As11TherapyState::Running;
    }
    if (event == "StandbyStarted" || event == "TherapyStop") {
        return As11TherapyState::Standby;
    }
    if (event == "MaskfitStarted" || event == "TestDriveStarted" ||
        event == "CalibrationStarted") {
        return As11TherapyState::Other;
    }
    return As11TherapyState::Unknown;
}

bool current_epoch_ms(int64_t &epoch_ms) {
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) != 0) return false;
    if (tv.tv_sec < VALID_TIME_MIN_EPOCH) return false;
    epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 +
               static_cast<int64_t>(tv.tv_usec / 1000);
    return true;
}

bool midpoint_epoch_ms(int64_t request_epoch_ms,
                       int64_t response_epoch_ms,
                       int64_t &epoch_ms) {
    const int64_t min_epoch_ms =
        static_cast<int64_t>(VALID_TIME_MIN_EPOCH) * 1000;
    if (request_epoch_ms < min_epoch_ms ||
        response_epoch_ms < request_epoch_ms) {
        return false;
    }
    epoch_ms = request_epoch_ms + (response_epoch_ms - request_epoch_ms) / 2;
    return true;
}

bool response_midpoint_epoch_ms(int64_t request_epoch_ms,
                                int64_t response_epoch_ms,
                                uint32_t request_ms,
                                uint32_t response_ms,
                                int64_t &epoch_ms) {
    const int64_t min_epoch_ms =
        static_cast<int64_t>(VALID_TIME_MIN_EPOCH) * 1000;
    if (response_epoch_ms >= min_epoch_ms &&
        (request_ms != 0 || response_ms != 0)) {
        const uint32_t elapsed_ms = response_ms - request_ms;
        epoch_ms = response_epoch_ms - elapsed_ms / 2;
        return epoch_ms >= min_epoch_ms;
    }

    return midpoint_epoch_ms(request_epoch_ms, response_epoch_ms, epoch_ms);
}

}  // namespace

void As11DeviceState::reset() {
    *this = As11DeviceState{};
}

bool As11DeviceState::set_availability(As11Availability availability,
                                       uint32_t now_ms) {
    if (availability_ == availability) return false;

    availability_ = availability;
    if (availability == As11Availability::Unavailable) {
        therapy_state_ = As11TherapyState::Unknown;
        runtime_event_state_ = As11TherapyState::Unknown;
        if (therapy_command_pending()) {
            clear_pending_therapy_command("device_unavailable", now_ms);
        }
    }
    return true;
}

const char *As11DeviceState::availability_name(
    As11Availability availability) {
    switch (availability) {
        case As11Availability::Available: return "available";
        case As11Availability::Unavailable: return "unavailable";
        case As11Availability::Unknown:
        default: return "unknown";
    }
}

void As11DeviceState::poll(uint32_t now_ms) {
    if (pending_therapy_target_ == As11TherapyTarget::None) return;
    if (static_cast<int32_t>(now_ms - pending_therapy_since_ms_) <
        static_cast<int32_t>(AC_AS11_THERAPY_CONFIRM_TIMEOUT_MS)) {
        return;
    }
    clear_pending_therapy_command("confirm_timeout", now_ms);
}

bool As11DeviceState::apply_status_get_response(RpcPayloadView payload,
                                                uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(
        doc, payload.data() ? payload.data() : "", payload.size());
    if (error) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) return false;

    bool updated = false;
    int32_t identity_number = 0;
    const ResmedIdentityQuery &identity = RESMED_IDENTITY;
    if (variant_to_int(result[identity.platform_id], identity_number)) {
        platform_id_ = identity_number;
        platform_id_valid_ = true;
        updated = true;
    }

    const ResmedDeviceProtocol *protocol = resmed_device_protocol(model());
    if (!protocol) return updated;

    std::string text;
    if (get_string(result, identity.product_name, text)) {
        product_name_ = text;
        updated = true;
    }
    if (get_string(result, identity.serial_number, text)) {
        serial_number_ = text;
        updated = true;
    }
    if (get_string(result, identity.software_identifier, text)) {
        software_identifier_ = text;
        updated = true;
    }
    if (get_string(result, identity.bootloader_identifier, text)) {
        bootloader_identifier_ = text;
        updated = true;
    }
    if (variant_to_int(result[identity.variant_id], identity_number)) {
        variant_id_ = identity_number;
        variant_id_valid_ = true;
        updated = true;
    }

    const ResmedRuntimeQuery &runtime = protocol->runtime;
    if (get_string(result, runtime.therapy_profile, text)) {
        active_therapy_profile_ = text;
        updated = true;
    }

    if (get_string(result, RESMED_MOTOR_RUNTIME, text)) {
        mhr_ = text;
        updated = true;
    }

    int32_t timezone = 0;
    if (protocol->timezone &&
        parse_timezone_offset_minutes(result[protocol->timezone], timezone)) {
        timezone_offset_minutes_ = timezone;
        timezone_offset_valid_ = true;
        updated = true;
    }

    if (get_string(result, runtime.running_mode, text)) {
        if (runtime.therapy_state) {
            rop_ = text;
        } else {
            update_rop(text, now_ms);
        }
        updated = true;
    }

    if (get_string(result, runtime.therapy_state, text)) {
        update_fg_state(text, now_ms);
        updated = true;
    }

    if (updated) {
        status_valid_ = true;
        status_updated_ms_ = now_ms;
    }
    return updated;
}

bool As11DeviceState::apply_datetime_response(
    RpcPayloadView payload,
    uint32_t now_ms,
    int64_t request_epoch_ms,
    int64_t response_epoch_ms,
    uint32_t request_ms,
    uint32_t response_ms) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(
        doc, payload.data() ? payload.data() : "", payload.size());
    if (error) return false;

    std::string text;
    if (!json_variant_to_string(doc["result"]["dateTime"], text)) {
        return false;
    }

    int64_t device_epoch_ms = 0;
    if (!parse_utc_iso8601_ms(text.c_str(), device_epoch_ms)) return false;

    device_datetime_ = text;
    clock_valid_ = true;
    clock_sample_ms_ = now_ms;
    clock_device_epoch_ms_ = device_epoch_ms;

    int64_t esp_epoch_ms = 0;
    if (response_midpoint_epoch_ms(request_epoch_ms, response_epoch_ms,
                                   request_ms, response_ms, esp_epoch_ms) ||
        current_epoch_ms(esp_epoch_ms)) {
        const int64_t offset = device_epoch_ms - esp_epoch_ms;
        clock_offset_ms_ = offset;
        clock_offset_valid_ = true;
    } else {
        clock_offset_valid_ = false;
    }
    return true;
}

bool As11DeviceState::estimate_device_epoch_ms(uint32_t now_ms,
                                               int64_t &epoch_ms) const {
    if (!clock_valid_) return false;

    // A loop's timestamp can slightly precede a clock response processed in it.
    const int32_t elapsed_ms = static_cast<int32_t>(now_ms - clock_sample_ms_);
    epoch_ms = clock_device_epoch_ms_ + static_cast<int64_t>(elapsed_ms);
    return true;
}

bool As11DeviceState::apply_activity_event_frame(const As11EventFrame &frame,
                                                 uint32_t now_ms) {
    const ResmedDeviceProtocol *protocol = resmed_device_protocol(model());
    if (protocol && protocol->runtime.therapy_state &&
        (frame.data_id == protocol->runtime.therapy_state ||
         frame.data_id == protocol->runtime.therapy_profile)) {
        bool updated = false;
        for (size_t i = 0; i < frame.event_count; ++i) {
            const As11EventRecord &event = frame.events[i];
            if (event.kind != As11EventRecordKind::ValueChange ||
                event.text_value.empty()) continue;

            if (frame.data_id == protocol->runtime.therapy_state) {
                // Initial values after subscribing are not transition times.
                const As11TherapyState previous =
                    runtime_event_subscription_id_ == frame.subscription_id
                        ? runtime_event_state_ : As11TherapyState::Unknown;
                update_fg_state(event.text_value, now_ms);
                runtime_event_subscription_id_ = frame.subscription_id;
                runtime_event_state_ = therapy_state_;

                if (previous != As11TherapyState::Unknown &&
                    previous != therapy_state_) {
                    last_therapy_transition_event_ = event.text_value;
                    last_therapy_transition_report_time_ = event.report_time;
                    last_therapy_transition_ms_ = now_ms;
                    last_therapy_transition_state_ = therapy_state_;
                }
            } else {
                active_therapy_profile_ = event.text_value;
            }
            last_activity_event_ = event.text_value;
            last_activity_event_report_time_ = event.report_time;
            last_activity_event_ms_ = now_ms;
            updated = true;
        }
        return updated;
    }

    if (!as11_event_data_id_is_activity(frame.data_id)) return false;
    bool updated = false;
    for (size_t i = 0; i < frame.event_count; ++i) {
        const As11EventRecord &event = frame.events[i];
        if (event.name.empty()) continue;
        As11TherapyState event_state = therapy_state_for_event(event.name);

        last_activity_event_ = event.name;
        last_activity_event_report_time_ = event.report_time;
        last_activity_event_ms_ = now_ms;
        if (event_state != As11TherapyState::Unknown) {
            last_therapy_transition_event_ = event.name;
            last_therapy_transition_report_time_ = event.report_time;
            last_therapy_transition_ms_ = now_ms;
            last_therapy_transition_state_ = event_state;
            therapy_state_ = event_state;
            status_valid_ = true;
            status_updated_ms_ = now_ms;
            confirm_pending_if_matched(now_ms);
        }
        updated = true;
    }
    return updated;
}

void As11DeviceState::mark_therapy_command_sent(const std::string &method,
                                                uint32_t now_ms) {
    As11TherapyTarget target = target_for_method(method);
    if (target == As11TherapyTarget::None) return;

    pending_therapy_target_ = target;
    pending_therapy_since_ms_ = now_ms;
    last_therapy_command_status_ = "sent";
}

void As11DeviceState::mark_therapy_command_response(const std::string &method,
                                                    bool is_error,
                                                    uint32_t now_ms) {
    As11TherapyTarget target = target_for_method(method);
    if (target == As11TherapyTarget::None) return;

    if (is_error) {
        clear_pending_therapy_command("error", now_ms);
        return;
    }

    if (pending_therapy_target_ == As11TherapyTarget::None) {
        pending_therapy_target_ = target;
        pending_therapy_since_ms_ = now_ms;
    }
    last_therapy_command_status_ = "accepted";
    confirm_pending_if_matched(now_ms);
}

void As11DeviceState::mark_therapy_command_timeout(const std::string &method,
                                                   uint32_t now_ms) {
    if (!is_therapy_command_method(method)) return;
    clear_pending_therapy_command("timeout", now_ms);
}

void As11DeviceState::clear_pending_therapy_command(const char *reason,
                                                    uint32_t now_ms) {
    (void)now_ms;
    pending_therapy_target_ = As11TherapyTarget::None;
    pending_therapy_since_ms_ = 0;
    last_therapy_command_status_ = reason ? reason : "";
}

bool As11DeviceState::is_therapy_command_method(const std::string &method) {
    return target_for_method(method) != As11TherapyTarget::None;
}

const char *As11DeviceState::therapy_state_name(As11TherapyState state) {
    switch (state) {
        case As11TherapyState::Standby: return "standby";
        case As11TherapyState::Running: return "running";
        case As11TherapyState::Other: return "other";
        case As11TherapyState::Unknown:
        default:
            return "unknown";
    }
}

const char *As11DeviceState::therapy_target_name(As11TherapyTarget target) {
    switch (target) {
        case As11TherapyTarget::Standby: return "standby";
        case As11TherapyTarget::Running: return "running";
        case As11TherapyTarget::None:
        default:
            return "none";
    }
}

void As11DeviceState::update_rop(const std::string &value, uint32_t now_ms) {
    rop_ = value;
    therapy_state_ = classify_rop(value);
    confirm_pending_if_matched(now_ms);
}

void As11DeviceState::update_fg_state(const std::string &value,
                                     uint32_t now_ms) {
    if (value == "Therapy") {
        therapy_state_ = As11TherapyState::Running;
    } else if (value == "Standby") {
        therapy_state_ = As11TherapyState::Standby;
    } else if (value == "Reset" || value == "ResetCompliance" ||
               value == "MaskFit" || value == "TestMode" ||
               value == "SystemError" || value == "Upgrade" ||
               value == "UpgradePreparation") {
        therapy_state_ = As11TherapyState::Other;
    } else {
        therapy_state_ = As11TherapyState::Unknown;
    }

    status_valid_ = true;
    status_updated_ms_ = now_ms;
    confirm_pending_if_matched(now_ms);
}

void As11DeviceState::confirm_pending_if_matched(uint32_t now_ms) {
    (void)now_ms;
    if (pending_therapy_target_ == As11TherapyTarget::None) return;
    if ((pending_therapy_target_ == As11TherapyTarget::Running &&
         therapy_state_ == As11TherapyState::Running) ||
        (pending_therapy_target_ == As11TherapyTarget::Standby &&
         therapy_state_ == As11TherapyState::Standby)) {
        pending_therapy_target_ = As11TherapyTarget::None;
        pending_therapy_since_ms_ = 0;
        last_therapy_command_status_ = "confirmed";
    }
}

}  // namespace aircannect
