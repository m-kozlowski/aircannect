#pragma once

#include <stdint.h>
#include <string>

namespace aircannect {

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
};

struct EventBrokerStats {
    uint32_t subscribe_requests = 0;
    uint32_t subscribe_successes = 0;
    uint32_t subscribe_errors = 0;
    uint32_t notifications = 0;
    uint32_t settings_history_changes = 0;
};

struct EventBrokerStatus {
    bool subscription_active = false;
    bool subscribe_pending = false;
    uint32_t subscription_id = 0;
    uint32_t last_notification_ms = 0;
};

class EventBroker {
public:
    EventCommand next_command(uint32_t now_ms);
    void mark_command_queued(EventCommandType type, uint32_t now_ms);
    void mark_command_deferred(uint32_t now_ms);
    void mark_command_timeout(uint32_t now_ms);
    void mark_command_cancelled(uint32_t now_ms);
    void mark_subscribe_response(bool is_error,
                                 uint32_t subscription_id,
                                 uint32_t now_ms);
    void mark_reattach(uint32_t now_ms);

    EventPublishResult publish_notification(const std::string &payload,
                                            uint32_t now_ms);
    void reset_counters();

    EventBrokerStatus status() const;
    const EventBrokerStats &stats() const { return stats_; }
    bool subscription_active() const { return subscription_active_; }
    uint32_t subscription_id() const { return subscription_id_; }

private:
    bool subscription_active_ = false;
    bool subscribe_pending_ = false;
    uint32_t subscription_id_ = 0;
    uint32_t next_subscribe_ms_ = 0;
    uint32_t last_notification_ms_ = 0;
    EventBrokerStats stats_ = {};
};

}  // namespace aircannect
