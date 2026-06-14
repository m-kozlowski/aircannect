#include "event_broker.h"

#include <ArduinoJson.h>

#include "board_can.h"

namespace aircannect {
namespace {

const char *const SETTINGS_HISTORY_CHANGE_DATA_ID =
    "SettingsHistoryChangeCount";

const char *const DEFAULT_EVENT_SUBSCRIBE_PARAMS =
    "{\"dataIds\":[\"SystemActivityEvents-FrequentActivityEvents\","
    "\"SystemActivityEvents-SporadicActivityEvents\","
    "\"TherapyEvents-RespiratoryEvents\","
    "\"SettingsHistoryChangeCount\"]}";

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

bool variant_to_uint32(JsonVariantConst value, uint32_t &out) {
    if (value.isNull()) return false;
    if (value.is<unsigned int>()) {
        out = value.as<unsigned int>();
        return true;
    }
    if (value.is<unsigned long>()) {
        out = static_cast<uint32_t>(value.as<unsigned long>());
        return true;
    }
    if (value.is<int>()) {
        const int parsed = value.as<int>();
        if (parsed < 0) return false;
        out = static_cast<uint32_t>(parsed);
        return true;
    }
    if (value.is<long>()) {
        const long parsed = value.as<long>();
        if (parsed < 0) return false;
        out = static_cast<uint32_t>(parsed);
        return true;
    }
    return false;
}

bool parse_event_notification(const std::string &payload,
                              As11EventFrame &frame) {
    frame = {};

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    std::string method;
    if (!variant_to_string(doc["method"], method) ||
        method != "EventNotification") {
        return false;
    }

    JsonObjectConst params = doc["params"].as<JsonObjectConst>();
    if (params.isNull()) return false;
    (void)variant_to_uint32(params["subscriptionId"], frame.subscription_id);
    if (!variant_to_string(params["dataId"], frame.data_id)) return false;

    JsonArrayConst events = params["events"].as<JsonArrayConst>();
    for (JsonObjectConst event : events) {
        if (frame.event_count >= AC_AS11_EVENT_FRAME_EVENTS_MAX) {
            frame.truncated = true;
            break;
        }
        As11EventRecord &record = frame.events[frame.event_count];
        if (!variant_to_string(event["event"], record.name)) continue;
        (void)variant_to_string(event["reportTime"], record.report_time);
        frame.event_count++;
    }
    return true;
}

bool settings_history_change_notification(const As11EventFrame &frame) {
    if (frame.data_id != SETTINGS_HISTORY_CHANGE_DATA_ID) return false;
    for (size_t i = 0; i < frame.event_count; ++i) {
        if (frame.events[i].name == "ValueChange") return true;
    }
    return false;
}

}  // namespace

EventCommand EventBroker::next_command(uint32_t now_ms) {
    EventCommand command;
    if (subscription_active_ || subscribe_pending_) return command;

    if (next_subscribe_ms_ == 0) {
        next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
        return command;
    }
    if (static_cast<int32_t>(now_ms - next_subscribe_ms_) < 0) {
        return command;
    }

    command.type = EventCommandType::Subscribe;
    command.params_json = DEFAULT_EVENT_SUBSCRIBE_PARAMS;
    return command;
}

void EventBroker::mark_command_queued(EventCommandType type,
                                      uint32_t now_ms) {
    if (type != EventCommandType::Subscribe) return;
    subscribe_pending_ = true;
    next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
    stats_.subscribe_requests++;
}

void EventBroker::mark_command_deferred(uint32_t now_ms) {
    next_subscribe_ms_ = now_ms + AC_RPC_DEFAULT_TIMEOUT_MS;
}

void EventBroker::mark_command_timeout(uint32_t now_ms) {
    subscribe_pending_ = false;
    subscription_active_ = false;
    subscription_id_ = 0;
    next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
    stats_.subscribe_errors++;
}

void EventBroker::mark_command_cancelled(uint32_t now_ms) {
    mark_command_timeout(now_ms);
}

void EventBroker::mark_subscribe_response(bool is_error,
                                          uint32_t subscription_id,
                                          uint32_t now_ms) {
    subscribe_pending_ = false;
    if (is_error || subscription_id == 0) {
        subscription_active_ = false;
        subscription_id_ = 0;
        next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
        stats_.subscribe_errors++;
        return;
    }

    subscription_active_ = true;
    subscription_id_ = subscription_id;
    next_subscribe_ms_ = 0;
    stats_.subscribe_successes++;
}

void EventBroker::mark_reattach(uint32_t now_ms) {
    subscribe_pending_ = false;
    subscription_active_ = false;
    subscription_id_ = 0;
    next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
}

EventPublishResult EventBroker::publish_notification(const std::string &payload,
                                                     uint32_t now_ms,
                                                     As11EventFrame &frame) {
    EventPublishResult result;
    if (!parse_event_notification(payload, frame)) return result;

    result.accepted = true;
    result.truncated = frame.truncated;
    last_notification_ms_ = now_ms;
    stats_.notifications++;
    if (frame.truncated) stats_.truncated_notifications++;

    result.settings_history_change =
        settings_history_change_notification(frame);
    if (result.settings_history_change) {
        stats_.settings_history_changes++;
    }
    for (FrameObserverSlot &slot : frame_observers_) {
        if (slot.observer) {
            slot.observer(slot.context, frame, now_ms);
        }
    }
    return result;
}

void EventBroker::set_frame_observer(EventFrameObserver observer,
                                     void *context) {
    for (FrameObserverSlot &slot : frame_observers_) {
        slot = {};
    }
    if (observer) {
        (void)add_frame_observer(observer, context);
    }
}

bool EventBroker::add_frame_observer(EventFrameObserver observer,
                                     void *context) {
    if (!observer) return false;
    for (FrameObserverSlot &slot : frame_observers_) {
        if (slot.observer == observer && slot.context == context) {
            return true;
        }
    }
    for (FrameObserverSlot &slot : frame_observers_) {
        if (!slot.observer) {
            slot.observer = observer;
            slot.context = context;
            return true;
        }
    }
    return false;
}

void EventBroker::remove_frame_observer(EventFrameObserver observer,
                                        void *context) {
    for (FrameObserverSlot &slot : frame_observers_) {
        if (slot.observer == observer && slot.context == context) {
            slot = {};
        }
    }
}

void EventBroker::reset_counters() {
    stats_ = {};
}

EventBrokerStatus EventBroker::status() const {
    EventBrokerStatus out;
    out.subscription_active = subscription_active_;
    out.subscribe_pending = subscribe_pending_;
    out.subscription_id = subscription_id_;
    out.last_notification_ms = last_notification_ms_;
    return out;
}

}  // namespace aircannect
