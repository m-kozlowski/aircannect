#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "as11_event_frame.h"

namespace aircannect {

static constexpr size_t AC_EVENT_FRAME_OBSERVERS_MAX = 4;
static constexpr size_t AC_EVENT_CONSUMERS_MAX = 4;

using EventConsumerHandle = int8_t;
static constexpr EventConsumerHandle EVENT_CONSUMER_INVALID = -1;

enum class EventAcquireStatus {
    Acquired,
    AlreadyActive,
    Full,
    Rejected,
};

struct EventAcquireResult {
    EventAcquireStatus status = EventAcquireStatus::Rejected;
    EventConsumerHandle handle = EVENT_CONSUMER_INVALID;
};

enum class EventCommandType {
    None,
    Subscribe,
};

struct EventCommand {
    EventCommandType type = EventCommandType::None;
    std::string params_json;
};

struct EventPublishResult {
    bool accepted = false;
    bool settings_history_change = false;
    bool truncated = false;
};

struct EventBrokerStats {
    uint32_t subscribe_requests = 0;
    uint32_t subscribe_successes = 0;
    uint32_t subscribe_errors = 0;
    uint32_t coverage_gaps = 0;
    uint32_t notifications = 0;
    uint32_t settings_history_changes = 0;
    uint32_t truncated_notifications = 0;
};

struct EventBrokerStatus {
    bool subscription_active = false;
    bool subscribe_pending = false;
    uint32_t subscription_id = 0;
    uint32_t subscription_generation = 0;
    uint32_t coverage_gap_count = 0;
    uint32_t last_notification_ms = 0;
};

using EventFrameObserver = void (*)(void *context,
                                    const As11EventFrame &frame,
                                    uint32_t now_ms);

class EventBroker {
public:
    EventCommand next_command(uint32_t now_ms);
    void mark_command_queued(EventCommandType type,
                             const std::string &params_json,
                             uint32_t now_ms);
    void mark_command_deferred(uint32_t now_ms);
    void mark_command_timeout(uint32_t now_ms);
    void mark_command_cancelled(uint32_t now_ms);
    void mark_subscribe_response(bool is_error,
                                 uint32_t subscription_id,
                                 uint32_t now_ms);
    void mark_reattach(uint32_t now_ms);

    EventAcquireResult acquire(const char *data_ids_csv);
    void release(EventConsumerHandle handle);
    bool consumer_active(EventConsumerHandle handle) const;

    EventPublishResult publish_notification(const std::string &payload,
                                            uint32_t now_ms,
                                            As11EventFrame &frame);
    EventPublishResult publish_notification(const char *payload,
                                            size_t payload_len,
                                            uint32_t now_ms,
                                            As11EventFrame &frame);
    void set_frame_observer(EventFrameObserver observer, void *context);
    bool add_frame_observer(EventFrameObserver observer, void *context);
    void remove_frame_observer(EventFrameObserver observer, void *context);
    void reset_counters();

    EventBrokerStatus status() const;
    const EventBrokerStats &stats() const { return stats_; }
    bool subscription_active() const { return subscription_active_; }
    uint32_t subscription_id() const { return subscription_id_; }

private:
    struct Consumer {
        bool active = false;
        std::string data_ids_csv;
    };

    struct FrameObserverSlot {
        EventFrameObserver observer = nullptr;
        void *context = nullptr;
    };

    bool refresh_desired_params();
    bool build_desired_params(std::string &params_json) const;
    void mark_desired_params_dirty();
    int find_free_slot() const;
    void note_subscription_gap(bool was_active);

    bool subscription_active_ = false;
    bool subscribe_pending_ = false;
    uint32_t subscription_id_ = 0;
    uint32_t subscription_generation_ = 0;
    uint32_t coverage_gap_count_ = 0;
    uint32_t next_subscribe_ms_ = 0;
    uint32_t last_notification_ms_ = 0;
    std::string active_params_json_;
    std::string pending_params_json_;
    std::string desired_params_json_;
    bool desired_params_dirty_ = true;
    bool desired_params_valid_ = false;
    Consumer consumers_[AC_EVENT_CONSUMERS_MAX];
    EventBrokerStats stats_ = {};
    FrameObserverSlot frame_observers_[AC_EVENT_FRAME_OBSERVERS_MAX];
};

}  // namespace aircannect
