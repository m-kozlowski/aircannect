#pragma once

#include <Arduino.h>
#include <memory>
#include <stdint.h>
#include <string>

#include "as11_device_state.h"
#include "as11_settings.h"
#include "board.h"
#include "can_datagram.h"
#include "can_driver.h"
#include "event_broker.h"
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
    Report,
    EdfRecorder,
};

enum class RpcEventKind {
    RpcResponse,
    RpcNotification,
    RpcUnmatched,
    DebugLog,
    BootNotification,
    InternalSettingsStateInvalidated,
    InternalSettingsStateUpdated,
    InternalDeviceStateUpdated,
    FramingError,
    Info,
};

using RpcPayloadRef = std::shared_ptr<const std::string>;
struct RpcEvent;
using RpcEventObserver = void (*)(void *context, const RpcEvent &event);

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

    bool reserve_reassembly_buffers();
    void poll();

    bool submit_raw_payload(const std::string &payload, RpcSource source);

    bool send_request(const std::string &method,
                      const std::string &params_json,
                      RpcSource source,
                      uint32_t timeout_ms = 0);
    bool send_request_with_id(const std::string &method,
                              const std::string &params_json,
                              RpcSource source,
                              uint32_t timeout_ms,
                              uint32_t &id);
    bool send_set_datetime_now(RpcSource source,
                               uint32_t timeout_ms = 0);

    bool next_event(RpcEvent &event);
    bool next_source_event(RpcSource source, RpcEvent &event);

    bool set_source_event_observer(RpcSource source,
                                   RpcEventObserver observer,
                                   void *context);

    void set_raw_rpc_events_enabled(bool enabled);

    void set_event_frame_observer(EventFrameObserver observer,
                                  void *context);
    bool add_event_frame_observer(EventFrameObserver observer,
                                  void *context);
    void remove_event_frame_observer(EventFrameObserver observer,
                                     void *context);

    EventAcquireResult acquire_events(const char *data_ids_csv);
    void release_events(EventConsumerHandle handle);
    bool event_consumer_active(EventConsumerHandle handle) const;

    StreamAcquireResult acquire_stream(const std::string &params_json,
                                       RpcSource source);
    StreamAcquireResult update_stream(StreamConsumerHandle handle,
                                      const std::string &params_json);
    void release_stream(StreamConsumerHandle handle);
    bool stream_consumer_active(StreamConsumerHandle handle) const;
    uint32_t stream_consumer_queue_drops(StreamConsumerHandle handle) const;
    bool stream_activity_active() const;
    bool stream_actual_active() const;
    size_t stream_accepted_data_id_count() const;
    bool stream_accepted_data_id(const char *data_id) const;
    bool stream_accepted_data_ids_cover(const char *data_ids_csv) const;
    const std::string &stream_accepted_data_ids_csv() const;
    void set_stream_frame_observer(StreamFrameObserver observer,
                                   void *context);
    bool next_stream_frame(StreamConsumerHandle handle,
                           StreamFrameRef &frame);

    void reset_stats();

    bool request_as11_healthcheck();
    bool request_as11_settings_refresh();
    bool recover_can(const char *reason);

    void cancel_requests_from_source(RpcSource source, const char *reason);
    void set_background_polls_suspended(bool suspended);
    void set_esp_reboot_quiesce(bool requested);
    bool esp_reboot_quiesced() const;

    const RpcArbiterStats &stats() const { return stats_; }
    RpcRuntimeStatus runtime_status() const;
    const CanDriver &can_driver() const { return can_; }
    const EventBroker &event_broker() const { return event_; }
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
        EventCommandType event_command = EventCommandType::None;
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
        EventCommandType event_command = EventCommandType::None;
        bool settings_refresh = false;
    };

    struct RawPassthroughRequest {
        bool active = false;
        uint32_t id = 0;
        uint32_t deadline_ms = 0;
        RpcSource source = RpcSource::Internal;
        StreamCommandType stream_command = StreamCommandType::None;
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
    void push_source_event(RpcSource target,
                           RpcEventKind kind,
                           const std::string &payload,
                           RpcSource source = RpcSource::Internal,
                           uint32_t id = 0);
    void push_source_event(RpcSource target,
                           RpcEventKind kind,
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
                                  StreamCommandType stream_command,
                                  uint32_t now);
    bool match_raw_passthrough(uint32_t id,
                               RawPassthroughRequest &request,
                               uint32_t now);
    bool match_raw_passthrough(uint32_t id,
                               RpcSource &source,
                               uint32_t now);
    void note_raw_stream_request(StreamCommandType command,
                                 const std::string &params_json);
    void note_raw_stream_response(StreamCommandType command,
                                  bool is_error);

    bool background_backoff_active(uint32_t now) const;
    void note_request_success(RpcSource source, uint32_t now);
    void note_request_timeout(RpcSource source, uint32_t now);
    bool request_allowed_during_esp_reboot_quiesce(
        const QueuedRequest &request) const;

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
    bool handle_event_notification(const char *payload, size_t payload_len);
    void handle_stream_notification(const char *payload, size_t payload_len);
    uint8_t source_id(RpcSource source) const;
    void handle_frame(const RawCanFrame &frame);

    void handle_rpc_payload(const char *payload, size_t payload_len);
    void handle_debug_payload(const char *payload, size_t payload_len);

    std::string format_boot_frame(const RawCanFrame &frame) const;
    const char *source_name(RpcSource source) const;

    enum class SourceEventQueue {
        None,
        ResmedOta,
    };

    struct SourceEventRoute {
        RpcSource source = RpcSource::Internal;
        SourceEventQueue queue = SourceEventQueue::None;
        RpcEventObserver observer = nullptr;
        void *observer_context = nullptr;
    };

    SourceEventRoute *source_event_route(RpcSource source);
    const SourceEventRoute *source_event_route(RpcSource source) const;
    bool dispatch_source_event(const SourceEventRoute &route,
                               const RpcEvent &event);
    bool push_source_event_queue(SourceEventQueue queue, RpcEvent &&event);
    bool pop_source_event_queue(SourceEventQueue queue, RpcEvent &event);

    CanDriver &can_;
    DatagramRx rpc_rx_{AC_STREAM_FRAME_RAW_MAX};
    DatagramRx log_rx_;
    FixedQueue<RpcEvent, AC_RPC_EVENT_QUEUE_DEPTH> events_;
    FixedQueue<RpcEvent, AC_RESMED_OTA_EVENT_QUEUE_DEPTH> resmed_ota_events_;
    SourceEventRoute source_event_routes_[3] = {
        {RpcSource::ResmedOta, SourceEventQueue::ResmedOta, nullptr,
         nullptr},
        {RpcSource::Report, SourceEventQueue::None, nullptr, nullptr},
        {RpcSource::EdfRecorder, SourceEventQueue::None, nullptr,
         nullptr},
    };

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

    EventBroker event_;
    StreamBroker stream_;
    As11DeviceState as11_state_;
    As11SettingsState as11_settings_;

    bool as11_healthcheck_initialized_ = false;

    uint32_t next_as11_identity_poll_ms_ = 0;
    uint32_t next_as11_status_poll_ms_ = 0;
    uint32_t next_as11_motor_poll_ms_ = 0;
    uint32_t next_as11_timezone_poll_ms_ = 0;
    uint32_t next_as11_clock_poll_ms_ = 0;

    bool background_polls_suspended_ = false;
    bool raw_rpc_events_enabled_ = false;
    bool esp_reboot_quiesce_requested_ = false;
    bool esp_reboot_quiesce_timeout_logged_ = false;

    uint32_t esp_reboot_quiesce_deadline_ms_ = 0;
    uint8_t consecutive_scheduler_timeouts_ = 0;
    uint32_t background_backoff_until_ms_ = 0;
};

}  // namespace aircannect
