#include "event_broker.h"

#include <ArduinoJson.h>
#include <string.h>

#include "as11_rpc.h"
#include "board_can.h"

namespace aircannect {
namespace {

const char *const SETTINGS_HISTORY_CHANGE_DATA_ID =
    "SettingsHistoryChangeCount";

const char *const DEFAULT_EVENT_SUBSCRIBE_PARAMS =
    "{\"dataIds\":[\"SystemActivityEvents-FrequentActivityEvents\","
    "\"SystemActivityEvents-SporadicActivityEvents\","
    "\"SettingsHistoryChangeCount\"]}";

bool settings_history_change_notification(const std::string &payload) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    const char *method = doc["method"].as<const char *>();
    if (!method || strcmp(method, "EventNotification") != 0) return false;

    JsonObjectConst params = doc["params"].as<JsonObjectConst>();
    if (params.isNull()) return false;
    const char *data_id = params["dataId"].as<const char *>();
    if (!data_id || strcmp(data_id, SETTINGS_HISTORY_CHANGE_DATA_ID) != 0) {
        return false;
    }

    JsonArrayConst events = params["events"].as<JsonArrayConst>();
    for (JsonObjectConst event : events) {
        const char *name = event["event"].as<const char *>();
        if (name && strcmp(name, "ValueChange") == 0) return true;
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

EventPublishResult EventBroker::publish_notification(
    const std::string &payload,
    uint32_t now_ms) {
    EventPublishResult result;
    if (!json_method_is(payload, "EventNotification")) return result;

    result.accepted = true;
    last_notification_ms_ = now_ms;
    stats_.notifications++;

    result.settings_history_change =
        settings_history_change_notification(payload);
    if (result.settings_history_change) {
        stats_.settings_history_changes++;
    }
    return result;
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
