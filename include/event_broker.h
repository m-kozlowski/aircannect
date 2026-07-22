#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "as11_event_frame.h"
#include "rpc_request_port.h"

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
    Quiesce,
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

    uint32_t quiesce_requests = 0;
    uint32_t quiesce_successes = 0;
    uint32_t quiesce_errors = 0;

    uint32_t coverage_gaps = 0;
    uint32_t notifications = 0;
    uint32_t settings_history_changes = 0;
    uint32_t truncated_notifications = 0;
};

struct EventBrokerStatus {
    bool subscription_active = false;
    bool subscribe_pending = false;
    bool quiesce_requested = false;
    bool quiesced = false;

    uint32_t subscription_id = 0;
    uint32_t subscription_generation = 0;
    uint32_t coverage_gap_count = 0;
    uint32_t last_notification_ms = 0;
};

using EventFrameObserver = void (*)(void *context,
                                    const As11EventFrame &frame,
                                    uint32_t now_ms);
using SettingsHistoryObserver = void (*)(void *context, uint32_t now_ms);

class EventBroker {
public:
    void poll(RpcRequestPort &rpc,
              uint32_t now_ms,
              bool background_suspended = false);
    void transport_reset(RpcRequestPort &rpc, uint32_t now_ms);

    EventCommand next_command(uint32_t now_ms);

    void mark_command_queued(EventCommandType type,
                             const std::string &params_json,
                             uint32_t now_ms);
    void mark_command_deferred(uint32_t now_ms);
    void mark_command_timeout(uint32_t now_ms);
    void mark_command_cancelled(uint32_t now_ms);
    bool accept_subscribe_response(const std::string &payload,
                                   uint32_t &subscription_id) const;
    void mark_subscribe_response(bool is_error,
                                 uint32_t subscription_id,
                                 uint32_t now_ms);
    void mark_reattach(uint32_t now_ms);
    void request_quiesce(uint32_t now_ms);
    void clear_quiesce(uint32_t now_ms);
    bool quiesced() const { return quiesced_; }

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

    bool add_frame_observer(EventFrameObserver observer, void *context);
    void set_settings_history_observer(SettingsHistoryObserver observer,
                                       void *context);

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
    void complete_command(const RpcRequestCompletion &completion,
                          uint32_t now_ms);
    uint32_t next_request_generation();
    void release_command_ticket(RpcRequestPort &rpc);

    bool subscription_active_ = false;
    bool subscribe_pending_ = false;
    bool pending_quiesce_ = false;
    bool quiesce_requested_ = false;
    bool quiesced_ = false;

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

    OperationTicket command_ticket_;
    EventCommandType command_type_ = EventCommandType::None;
    uint32_t request_generation_ = 0;

    Consumer consumers_[AC_EVENT_CONSUMERS_MAX];
    EventBrokerStats stats_ = {};
    FrameObserverSlot frame_observers_[AC_EVENT_FRAME_OBSERVERS_MAX];
    SettingsHistoryObserver settings_history_observer_ = nullptr;
    void *settings_history_observer_context_ = nullptr;
};

}  // namespace aircannect
