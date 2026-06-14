#include "event_broker.h"

#include <ArduinoJson.h>
#include <string.h>

#include "board_can.h"

namespace aircannect {
namespace {

const char *const SETTINGS_HISTORY_CHANGE_DATA_ID =
    "SettingsHistoryChangeCount";

const char *const BASE_EVENT_DATA_IDS[] = {
    "SystemActivityEvents-FrequentActivityEvents",
    "SystemActivityEvents-SporadicActivityEvents",
    SETTINGS_HISTORY_CHANGE_DATA_ID,
};

bool csv_contains_data_id(const std::string &csv, const char *data_id) {
    if (!data_id || !*data_id) return true;
    const size_t id_len = strlen(data_id);
    size_t pos = 0;
    while (pos < csv.size()) {
        const size_t end = csv.find(',', pos);
        const size_t len =
            (end == std::string::npos) ? csv.size() - pos : end - pos;
        if (len == id_len && csv.compare(pos, len, data_id) == 0) {
            return true;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

bool add_data_id(std::string &csv, const char *data_id) {
    if (!data_id || !*data_id) return true;
    if (strchr(data_id, ',') || strchr(data_id, '"') || strchr(data_id, '\\')) {
        return false;
    }
    if (csv_contains_data_id(csv, data_id)) return true;
    if (!csv.empty()) csv += ',';
    csv += data_id;
    return true;
}

bool merge_data_ids(std::string &csv, const char *data_ids_csv) {
    if (!data_ids_csv) return true;
    const char *pos = data_ids_csv;
    while (*pos) {
        while (*pos == ',' || *pos == ' ' || *pos == '\t') pos++;
        const char *start = pos;
        while (*pos && *pos != ',') pos++;
        const char *end = pos;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > start) {
            std::string token(start, static_cast<size_t>(end - start));
            if (!add_data_id(csv, token.c_str())) return false;
        }
        if (*pos == ',') pos++;
    }
    return true;
}

void build_subscribe_params_from_csv(const std::string &csv,
                                     std::string &params_json) {
    params_json = "{\"dataIds\":[";
    bool first = true;
    size_t pos = 0;
    while (pos < csv.size()) {
        const size_t end = csv.find(',', pos);
        const size_t len =
            (end == std::string::npos) ? csv.size() - pos : end - pos;
        if (len > 0) {
            if (!first) params_json += ',';
            first = false;
            params_json += '"';
            params_json.append(csv, pos, len);
            params_json += '"';
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    params_json += "]}";
}

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

bool variant_to_int32(JsonVariantConst value, int32_t &out) {
    if (value.isNull()) return false;
    if (value.is<int>()) {
        out = value.as<int>();
        return true;
    }
    if (value.is<long>()) {
        const long parsed = value.as<long>();
        if (parsed < INT32_MIN || parsed > INT32_MAX) return false;
        out = static_cast<int32_t>(parsed);
        return true;
    }
    if (value.is<unsigned int>()) {
        const unsigned int parsed = value.as<unsigned int>();
        if (parsed > static_cast<unsigned int>(INT32_MAX)) return false;
        out = static_cast<int32_t>(parsed);
        return true;
    }
    if (value.is<unsigned long>()) {
        const unsigned long parsed = value.as<unsigned long>();
        if (parsed > static_cast<unsigned long>(INT32_MAX)) return false;
        out = static_cast<int32_t>(parsed);
        return true;
    }
    return false;
}

bool parse_event_duration_ms(JsonObjectConst event, int32_t &duration_ms) {
    int32_t value = 0;
    if (variant_to_int32(event["durationMs"], value) ||
        variant_to_int32(event["duration_ms"], value) ||
        variant_to_int32(event["durationMilliseconds"], value)) {
        if (value < 0) return false;
        duration_ms = value;
        return true;
    }

    if (variant_to_int32(event["durationSeconds"], value) ||
        variant_to_int32(event["duration_s"], value)) {
        if (value < 0 || value > (INT32_MAX / 1000)) return false;
        duration_ms = value * 1000;
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
        record.has_duration =
            parse_event_duration_ms(event, record.duration_ms);
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
    if (subscribe_pending_) return command;

    if (!refresh_desired_params()) return command;

    if (subscription_active_ &&
        desired_params_json_ == active_params_json_) {
        return command;
    }

    if (!subscription_active_ && next_subscribe_ms_ == 0) {
        next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
        return command;
    }
    if (next_subscribe_ms_ != 0 &&
        static_cast<int32_t>(now_ms - next_subscribe_ms_) < 0) {
        return command;
    }

    command.type = EventCommandType::Subscribe;
    command.params_json = desired_params_json_;
    return command;
}

void EventBroker::mark_command_queued(EventCommandType type,
                                      const std::string &params_json,
                                      uint32_t now_ms) {
    if (type != EventCommandType::Subscribe) return;
    subscribe_pending_ = true;
    pending_params_json_ = params_json;
    next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
    stats_.subscribe_requests++;
}

void EventBroker::mark_command_deferred(uint32_t now_ms) {
    next_subscribe_ms_ = now_ms + AC_RPC_DEFAULT_TIMEOUT_MS;
}

void EventBroker::mark_command_timeout(uint32_t now_ms) {
    const bool was_active = subscription_active_;
    note_subscription_gap(was_active);
    subscribe_pending_ = false;
    subscription_active_ = false;
    subscription_id_ = 0;
    active_params_json_.clear();
    pending_params_json_.clear();
    next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
    stats_.subscribe_errors++;
}

void EventBroker::mark_command_cancelled(uint32_t now_ms) {
    mark_command_timeout(now_ms);
}

void EventBroker::mark_subscribe_response(bool is_error,
                                          uint32_t subscription_id,
                                          uint32_t now_ms) {
    const bool was_active = subscription_active_;
    subscribe_pending_ = false;
    if (is_error || subscription_id == 0) {
        note_subscription_gap(was_active);
        subscription_active_ = false;
        subscription_id_ = 0;
        active_params_json_.clear();
        pending_params_json_.clear();
        next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
        stats_.subscribe_errors++;
        return;
    }

    subscription_active_ = true;
    subscription_id_ = subscription_id;
    subscription_generation_++;
    if (pending_params_json_.empty()) {
        if (refresh_desired_params()) {
            active_params_json_ = desired_params_json_;
        } else {
            active_params_json_.clear();
        }
    } else {
        active_params_json_ = pending_params_json_;
    }
    pending_params_json_.clear();
    next_subscribe_ms_ = 0;
    stats_.subscribe_successes++;
}

void EventBroker::mark_reattach(uint32_t now_ms) {
    const bool was_active = subscription_active_;
    note_subscription_gap(was_active);
    subscribe_pending_ = false;
    subscription_active_ = false;
    subscription_id_ = 0;
    active_params_json_.clear();
    pending_params_json_.clear();
    next_subscribe_ms_ = now_ms + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
}

EventAcquireResult EventBroker::acquire(const char *data_ids_csv) {
    EventAcquireResult result;
    std::string parsed;
    if (!merge_data_ids(parsed, data_ids_csv) || parsed.empty()) {
        result.status = EventAcquireStatus::Rejected;
        return result;
    }

    const int slot = find_free_slot();
    if (slot < 0) {
        result.status = EventAcquireStatus::Full;
        return result;
    }

    consumers_[slot].active = true;
    consumers_[slot].data_ids_csv = parsed;
    mark_desired_params_dirty();
    const bool already_active =
        subscription_active_ &&
        refresh_desired_params() &&
        desired_params_json_ == active_params_json_;
    result.status = already_active
        ? EventAcquireStatus::AlreadyActive
        : EventAcquireStatus::Acquired;
    result.handle = static_cast<EventConsumerHandle>(slot);
    return result;
}

void EventBroker::release(EventConsumerHandle handle) {
    if (handle < 0 ||
        handle >= static_cast<EventConsumerHandle>(AC_EVENT_CONSUMERS_MAX)) {
        return;
    }
    if (!consumers_[handle].active) return;
    consumers_[handle] = {};
    mark_desired_params_dirty();
}

bool EventBroker::consumer_active(EventConsumerHandle handle) const {
    if (handle < 0 ||
        handle >= static_cast<EventConsumerHandle>(AC_EVENT_CONSUMERS_MAX)) {
        return false;
    }
    return consumers_[handle].active;
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
    out.subscription_generation = subscription_generation_;
    out.coverage_gap_count = coverage_gap_count_;
    out.last_notification_ms = last_notification_ms_;
    return out;
}

bool EventBroker::refresh_desired_params() {
    if (!desired_params_dirty_) return desired_params_valid_;

    std::string params_json;
    desired_params_valid_ = build_desired_params(params_json);
    if (desired_params_valid_) {
        desired_params_json_ = params_json;
    } else {
        desired_params_json_.clear();
    }
    desired_params_dirty_ = false;
    return desired_params_valid_;
}

bool EventBroker::build_desired_params(std::string &params_json) const {
    std::string csv;
    for (const char *data_id : BASE_EVENT_DATA_IDS) {
        if (!add_data_id(csv, data_id)) return false;
    }
    for (const Consumer &consumer : consumers_) {
        if (!consumer.active) continue;
        if (!merge_data_ids(csv, consumer.data_ids_csv.c_str())) {
            return false;
        }
    }
    build_subscribe_params_from_csv(csv, params_json);
    return true;
}

void EventBroker::mark_desired_params_dirty() {
    desired_params_dirty_ = true;
}

int EventBroker::find_free_slot() const {
    for (size_t i = 0; i < AC_EVENT_CONSUMERS_MAX; ++i) {
        if (!consumers_[i].active) return static_cast<int>(i);
    }
    return -1;
}

void EventBroker::note_subscription_gap(bool was_active) {
    if (!was_active) return;
    coverage_gap_count_++;
    stats_.coverage_gaps++;
}

}  // namespace aircannect
