#include "as11_device_state.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "board.h"
#include "calendar_utils.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr time_t VALID_TIME_MIN_EPOCH = 1609459200;

bool variant_to_string(JsonVariantConst value, std::string &out) {
    if (value.isNull()) return false;
    if (value.is<const char *>()) {
        out = value.as<const char *>();
        return true;
    }
    if (value.is<int>()) {
        out = std::to_string(value.as<int>());
        return true;
    }
    if (value.is<unsigned int>()) {
        out = std::to_string(value.as<unsigned int>());
        return true;
    }
    if (value.is<long>()) {
        out = std::to_string(value.as<long>());
        return true;
    }
    if (value.is<unsigned long>()) {
        out = std::to_string(value.as<unsigned long>());
        return true;
    }
    if (value.is<bool>()) {
        out = value.as<bool>() ? "true" : "false";
        return true;
    }
    return false;
}

bool get_string(JsonObjectConst object, const char *name, std::string &out) {
    return variant_to_string(object[name], out);
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

bool utc_fields_to_epoch_ms(int year,
                            int month,
                            int day,
                            int hour,
                            int minute,
                            int second,
                            int millisecond,
                            int64_t &epoch_ms) {
    if (year < 2020 || month < 1 || month > 12 || day < 1 ||
        day > calendar_days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 ||
        millisecond < 0 || millisecond > 999) {
        return false;
    }
    const int64_t days =
        calendar_days_from_civil(year, static_cast<unsigned>(month),
                                 static_cast<unsigned>(day));
    const int64_t seconds = days * 86400 +
                            static_cast<int64_t>(hour) * 3600 +
                            static_cast<int64_t>(minute) * 60 + second;
    if (seconds < static_cast<int64_t>(VALID_TIME_MIN_EPOCH)) return false;
    epoch_ms = seconds * 1000 + millisecond;
    return true;
}

bool parse_datetime_epoch_ms(const std::string &datetime, int64_t &epoch_ms) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    if (sscanf(datetime.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &year, &month, &day, &hour, &minute, &second,
               &consumed) != 6) {
        return false;
    }

    int millisecond = 0;
    const char *p = datetime.c_str() + consumed;
    if (*p == '.') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                millisecond = millisecond * 10 + (*p - '0');
            }
            digits++;
            p++;
        }
        if (digits == 0) return false;
        while (digits < 3) {
            millisecond *= 10;
            digits++;
        }
    }
    if (*p != 'Z' || p[1] != 0) return false;
    return utc_fields_to_epoch_ms(year, month, day, hour, minute, second,
                                  millisecond, epoch_ms);
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

}  // namespace

const char *as11_identity_get_params_json() {
    return "[\"_PNA\",\"_SRN\",\"_SID\",\"_MID\",\"_VID\"]";
}

const char *as11_runtime_get_params_json() {
    return "[\"_MOP\",\"_ROP\"]";
}

const char *as11_motor_runtime_get_params_json() {
    return "[\"_MHR\"]";
}

const char *as11_timezone_get_params_json() {
    return "[\"_TZO\"]";
}

bool as11_parse_event_subscription_response(const std::string &payload,
                                            bool require_activity_selectors,
                                            uint32_t &subscription_id) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) return false;

    int32_t signed_id = 0;
    if (variant_to_int(result["subscriptionId"], signed_id) &&
        signed_id >= 0) {
        subscription_id = static_cast<uint32_t>(signed_id);
    } else if (result["subscriptionId"].is<unsigned int>()) {
        subscription_id = result["subscriptionId"].as<unsigned int>();
    } else {
        return false;
    }

    JsonArrayConst ids = result["dataIds"].as<JsonArrayConst>();
    if (!require_activity_selectors) return ids.isNull() || ids.size() == 0;
    if (ids.isNull()) return true;

    bool saw_supported_selector = false;
    bool accepted_supported_selector = false;
    for (JsonObjectConst item : ids) {
        std::string data_id;
        if (!variant_to_string(item["dataId"], data_id)) continue;
        if (data_id != "SystemActivityEvents-FrequentActivityEvents" &&
            data_id != "SystemActivityEvents-SporadicActivityEvents") {
            continue;
        }
        saw_supported_selector = true;
        if (item["valid"].as<bool>()) accepted_supported_selector = true;
    }
    return saw_supported_selector && accepted_supported_selector;
}

void As11DeviceState::reset() {
    *this = As11DeviceState{};
}

void As11DeviceState::poll(uint32_t now_ms) {
    if (pending_therapy_target_ == As11TherapyTarget::None) return;
    if (static_cast<int32_t>(now_ms - pending_therapy_since_ms_) <
        static_cast<int32_t>(AC_AS11_THERAPY_CONFIRM_TIMEOUT_MS)) {
        return;
    }
    clear_pending_therapy_command("confirm_timeout", now_ms);
}

bool As11DeviceState::apply_status_get_response(const std::string &payload,
                                                uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) return false;

    bool updated = false;
    std::string text;
    if (get_string(result, "_PNA", text)) {
        product_name_ = text;
        updated = true;
    }
    if (get_string(result, "_SRN", text)) {
        serial_number_ = text;
        updated = true;
    }
    if (get_string(result, "_SID", text)) {
        software_identifier_ = text;
        updated = true;
    }
    int32_t identity_number = 0;
    if (variant_to_int(result["_MID"], identity_number)) {
        platform_id_ = identity_number;
        platform_id_valid_ = true;
        updated = true;
    }
    if (variant_to_int(result["_VID"], identity_number)) {
        variant_id_ = identity_number;
        variant_id_valid_ = true;
        updated = true;
    }
    if (get_string(result, "_MOP", text)) {
        active_therapy_profile_ = text;
        updated = true;
    }
    if (get_string(result, "_MHR", text)) {
        mhr_ = text;
        updated = true;
    }

    int32_t timezone = 0;
    if (parse_timezone_offset_minutes(result["_TZO"], timezone)) {
        timezone_offset_minutes_ = timezone;
        timezone_offset_valid_ = true;
        updated = true;
    }

    if (get_string(result, "_ROP", text) || get_string(result, "ROP", text)) {
        update_rop(text, now_ms);
        updated = true;
    }

    if (updated) {
        status_valid_ = true;
        status_updated_ms_ = now_ms;
    }
    return updated;
}

bool As11DeviceState::apply_datetime_response(
    const std::string &payload,
    uint32_t now_ms,
    int64_t request_epoch_ms,
    int64_t response_epoch_ms) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    std::string text;
    if (!variant_to_string(doc["result"]["dateTime"], text)) return false;
    device_datetime_ = text;
    clock_valid_ = true;
    clock_sample_ms_ = now_ms;
    int64_t device_epoch_ms = 0;
    int64_t esp_epoch_ms = 0;
    if (parse_datetime_epoch_ms(text, device_epoch_ms) &&
        (midpoint_epoch_ms(request_epoch_ms, response_epoch_ms,
                           esp_epoch_ms) ||
         current_epoch_ms(esp_epoch_ms))) {
        const int64_t offset = device_epoch_ms - esp_epoch_ms;
        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            clock_offset_ms_ = static_cast<int32_t>(offset);
            clock_offset_valid_ = true;
        } else {
            clock_offset_valid_ = false;
        }
    } else {
        clock_offset_valid_ = false;
    }
    return true;
}

bool As11DeviceState::apply_activity_subscription_response(
    const std::string &payload,
    uint32_t now_ms,
    uint32_t &subscription_id) {
    (void)now_ms;
    return as11_parse_event_subscription_response(payload, true,
                                                  subscription_id);
}

bool As11DeviceState::apply_activity_event_frame(const As11EventFrame &frame,
                                                 uint32_t now_ms) {
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
