#pragma once

#include <Arduino.h>
#include <memory>
#include <stdint.h>
#include <string>

#include "board.h"
#include "as11_device_state.h"
#include "as11_settings.h"
#include "can_datagram.h"
#include "can_driver.h"
#include "fixed_queue.h"
#include "stream_broker.h"

namespace aircannect {

enum class RpcSource {
    Console,
    Tcp,
    HttpApi,
    Scheduler,
    Internal,
    ResmedOta,
    Sink,
};

enum class RpcEventKind {
    RpcResponse,
    RpcNotification,
    RpcUnmatched,
    DebugLog,
    BootNotification,
    InternalSettingsStateInvalidated,
    InternalSettingsStateUpdated,
    FramingError,
    Info,
};

using RpcPayloadRef = std::shared_ptr<const std::string>;

inline RpcPayloadRef make_rpc_payload_ref(std::string payload) {
    return std::make_shared<const std::string>(std::move(payload));
}

struct RpcEvent {
    RpcEventKind kind = RpcEventKind::Info;
    RpcSource source = RpcSource::Internal;
    uint32_t id = 0;
    RpcPayloadRef payload;

    const std::string &payload_text() const {
        static const std::string empty;
        return payload ? *payload : empty;
    }

    const char *payload_c_str() const {
        return payload ? payload->c_str() : "";
    }
};

struct RpcArbiterStats {
    uint32_t rpc_datagrams = 0;
    uint32_t rpc_responses = 0;
    uint32_t rpc_notifications = 0;
    uint32_t rpc_unmatched = 0;
    uint32_t rpc_framing_errors = 0;
    uint32_t log_datagrams = 0;
    uint32_t log_framing_errors = 0;
    uint32_t boot_notifications = 0;

    uint32_t queued_requests = 0;
    uint32_t dispatched_requests = 0;
    uint32_t request_timeouts = 0;
    uint32_t request_queue_drops = 0;
    uint32_t request_cancellations = 0;
    uint32_t request_dispatch_retries = 0;
    uint32_t background_backoffs = 0;

    uint32_t event_drops = 0;

    uint32_t stream_start_requests = 0;
    uint32_t stream_stop_requests = 0;
    uint32_t stream_notifications = 0;
    uint32_t stream_fanout_drops = 0;
    uint32_t stream_consumer_rejects = 0;
    uint32_t stream_command_deferred = 0;
    uint32_t stream_command_errors = 0;
    uint32_t stream_parse_errors = 0;
    uint32_t stream_pool_exhaustions = 0;
    uint32_t stream_truncated_frames = 0;

    uint32_t event_subscribe_errors = 0;
    uint32_t event_notifications = 0;
    uint32_t activity_state_events = 0;
};

struct RpcRuntimeStatus {
    uint32_t stats_elapsed_ms = 0;
    size_t request_queue_depth = 0;
    uint32_t pending_request_id = 0;
    uint32_t dispatch_retry_id = 0;
    uint32_t background_backoff_ms = 0;
    bool event_subscription_active = false;
    uint32_t event_subscription_id = 0;
    uint32_t boot_notifications = 0;
    uint32_t last_boot_notification_age_ms = 0;
    std::string last_boot_notification;
};

class RpcArbiter {
public:
    explicit RpcArbiter(CanDriver &can);

    void poll();

    bool submit_raw_payload(const std::string &payload, RpcSource source);
    bool send_request(const std::string &method,
                      const std::string &params_json,
                      RpcSource source,
                      uint32_t timeout_ms = 0);
    bool send_set_datetime_now(RpcSource source,
                               uint32_t timeout_ms = 0);
    bool next_event(RpcEvent &event);
    bool next_resmed_ota_event(RpcEvent &event);
    void set_raw_rpc_events_enabled(bool enabled);

    StreamAcquireResult acquire_stream(const std::string &params_json,
                                       RpcSource source);
    StreamAcquireResult update_stream(StreamConsumerHandle handle,
                                      const std::string &params_json);
    void release_stream(StreamConsumerHandle handle);
    bool stream_consumer_active(StreamConsumerHandle handle) const;
    uint32_t stream_consumer_queue_drops(StreamConsumerHandle handle) const;
    bool stream_activity_active() const;
    void set_stream_frame_observer(StreamFrameObserver observer,
                                   void *context);
    bool next_stream_frame(StreamConsumerHandle handle,
                           StreamFrameRef &frame);
    bool next_stream_payload(StreamConsumerHandle handle,
                             std::string &payload);

    void reset_stats();

    bool request_as11_healthcheck();
    bool request_as11_settings_refresh();
    bool recover_can(const char *reason);
    void cancel_requests_from_source(RpcSource source, const char *reason);
    void set_background_polls_suspended(bool suspended);

    const RpcArbiterStats &stats() const { return stats_; }
    RpcRuntimeStatus runtime_status() const;
    const CanDriver &can_driver() const { return can_; }
    const StreamBroker &stream_broker() const { return stream_; }
    const As11DeviceState &as11_state() const { return as11_state_; }
    const As11SettingsState &as11_settings() const { return as11_settings_; }

private:
    struct PendingRequest {
        bool active = false;
        uint32_t id = 0;
        uint32_t deadline_ms = 0;
        int64_t dispatch_epoch_ms = 0;
        RpcSource source = RpcSource::Internal;
        std::string method;
        StreamCommandType stream_command = StreamCommandType::None;
        bool settings_refresh = false;
    };

    struct QueuedRequest {
        uint32_t id = 0;
        uint32_t timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
        RpcSource source = RpcSource::Internal;
        std::string method;
        std::string params_json;
        bool set_datetime_now = false;
        StreamCommandType stream_command = StreamCommandType::None;
        bool settings_refresh = false;
    };

    struct RawPassthroughRequest {
        bool active = false;
        uint32_t id = 0;
        uint32_t deadline_ms = 0;
        RpcSource source = RpcSource::Internal;
    };

    static constexpr size_t RAW_PASSTHROUGH_PENDING_MAX = 8;

    void push_event(RpcEventKind kind,
                    const std::string &payload,
                    RpcSource source = RpcSource::Internal,
                    uint32_t id = 0);
    void push_event(RpcEventKind kind,
                    RpcPayloadRef payload,
                    RpcSource source = RpcSource::Internal,
                    uint32_t id = 0);
    void push_resmed_ota_event(RpcEventKind kind,
                               const std::string &payload,
                               RpcSource source = RpcSource::Internal,
                               uint32_t id = 0);
    void push_resmed_ota_event(RpcEventKind kind,
                               RpcPayloadRef payload,
                               RpcSource source = RpcSource::Internal,
                               uint32_t id = 0);

    bool enqueue_request(QueuedRequest &request);
    bool enqueue_payload_frames(const std::string &payload, RpcSource source);
    bool enqueue_encoded_frames(const std::vector<DatagramFrame> &frames);
    void cancel_pending_request(const char *reason);
    void cancel_queued_request(const QueuedRequest &request,
                               const char *reason);
    void cancel_all_requests(const char *reason);

    void expire_raw_passthrough(uint32_t now);
    void remember_raw_passthrough(uint32_t id,
                                  RpcSource source,
                                  uint32_t now);
    bool match_raw_passthrough(uint32_t id,
                               RpcSource &source,
                               uint32_t now);

    bool background_backoff_active(uint32_t now) const;
    void note_request_success(RpcSource source, uint32_t now);
    void note_request_timeout(RpcSource source, uint32_t now);
    void schedule_as11_identity_refresh(uint32_t now, uint32_t delay_ms);
    void schedule_as11_status_refresh(uint32_t now, uint32_t delay_ms);
    void schedule_as11_motor_refresh(uint32_t now, uint32_t delay_ms);
    void schedule_as11_timezone_refresh(uint32_t now, uint32_t delay_ms);
    void update_as11_motor_refresh_after_state(uint32_t now);
    void dispatch_next_request();
    void check_pending_timeout();
    void poll_event_subscription();
    void poll_stream_subscription();
    void poll_as11_healthcheck();

    void handle_matched_response(const std::string &payload);
    bool handle_event_notification(const std::string &payload);
    void handle_stream_notification(const std::string &payload);
    uint8_t source_id(RpcSource source) const;
    void handle_frame(const RawCanFrame &frame);
    void handle_rpc_payload(std::string payload);
    void handle_debug_payload(std::string payload);
    std::string format_boot_frame(const RawCanFrame &frame) const;
    const char *source_name(RpcSource source) const;

    CanDriver &can_;
    DatagramRx rpc_rx_;
    DatagramRx log_rx_;
    FixedQueue<RpcEvent, AC_RPC_EVENT_QUEUE_DEPTH> events_;
    FixedQueue<RpcEvent, AC_RESMED_OTA_EVENT_QUEUE_DEPTH> resmed_ota_events_;

    FixedQueue<QueuedRequest, AC_RPC_REQUEST_QUEUE_DEPTH> requests_;
    PendingRequest pending_;
    RawPassthroughRequest raw_passthrough_[RAW_PASSTHROUGH_PENDING_MAX];
    QueuedRequest dispatch_retry_;
    bool dispatch_retry_active_ = false;
    uint32_t dispatch_retry_deadline_ms_ = 0;
    uint32_t next_dispatch_retry_ms_ = 0;
    RpcArbiterStats stats_ = {};
    uint32_t next_rpc_id_ = 0;
    uint32_t last_integrated_tx_ms_ = 0;
    uint32_t stats_started_ms_ = 0;
    uint32_t boot_notifications_seen_ = 0;
    uint32_t last_boot_notification_ms_ = 0;
    std::string last_boot_notification_;

    StreamBroker stream_;
    As11DeviceState as11_state_;
    As11SettingsState as11_settings_;

    bool event_subscription_active_ = false;
    uint32_t event_subscription_id_ = 0;
    uint32_t next_event_subscribe_ms_ = 0;

    bool as11_healthcheck_initialized_ = false;
    uint32_t next_as11_identity_poll_ms_ = 0;
    uint32_t next_as11_status_poll_ms_ = 0;
    uint32_t next_as11_motor_poll_ms_ = 0;
    uint32_t next_as11_timezone_poll_ms_ = 0;
    uint32_t next_as11_clock_poll_ms_ = 0;
    bool background_polls_suspended_ = false;
    bool raw_rpc_events_enabled_ = false;
    uint8_t consecutive_scheduler_timeouts_ = 0;
    uint32_t background_backoff_until_ms_ = 0;
};

}  // namespace aircannect
