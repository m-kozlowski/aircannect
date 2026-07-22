#pragma once

#include <Arduino.h>
#include <memory>
#include <stdint.h>
#include <string>

#include "board.h"
#include "can_datagram.h"
#include "can_driver.h"
#include "fixed_queue.h"
#include "rpc_request_port.h"
#include "stream_broker.h"

namespace aircannect {

enum class RpcEventKind {
    RpcResponse,
    RpcNotification,
    RpcUnmatched,
    DebugLog,
    BootNotification,
    FramingError,
    Info,
};

using RpcPayloadRef = std::shared_ptr<const std::string>;
struct RpcEvent;
using RpcEventObserver = void (*)(void *context, const RpcEvent &event);
using RpcNotificationObserver = void (*)(void *context,
                                         const char *payload,
                                         size_t payload_len,
                                         uint32_t now_ms);

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
    uint32_t deferred_payloads = 0;
    uint32_t deferred_payload_drops = 0;
    uint32_t deferred_payload_alloc_failures = 0;

    uint32_t queued_requests = 0;
    uint32_t dispatched_requests = 0;
    uint32_t request_timeouts = 0;
    uint32_t request_queue_drops = 0;
    uint32_t request_cancellations = 0;
    uint32_t request_dispatch_retries = 0;
    uint32_t background_backoffs = 0;

    uint32_t event_drops = 0;
};

struct RpcRuntimeStatus {
    uint32_t stats_elapsed_ms = 0;
    size_t request_queue_depth = 0;
    size_t payload_queue_depth = 0;
    uint32_t pending_request_id = 0;
    uint32_t dispatch_retry_id = 0;
    uint32_t background_backoff_ms = 0;
    uint32_t boot_notifications = 0;
    uint32_t last_boot_notification_age_ms = 0;
    std::string last_boot_notification;
};

class RpcArbiter final : public RpcRequestPort {
public:
    RpcArbiter(CanDriver &can, StreamBroker &stream_broker);

    // Lifecycle and CAN pump
    bool reserve_reassembly_buffers();
    void poll();
    size_t drain_can_rx();

    // RPC submission
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
    OperationSubmission request(const RpcRequestCommand &command) override;
    bool cancel(OperationTicket ticket) override;
    bool take_completion(OperationTicket ticket,
                         RpcRequestCompletion &completion) override;

    // Event routing
    bool next_event(RpcEvent &event);
    bool next_source_event(RpcSource source, RpcEvent &event);

    bool set_source_event_observer(RpcSource source,
                                   RpcEventObserver observer,
                                   void *context);

    void set_raw_rpc_events_enabled(bool enabled);
    void set_event_notification_observer(RpcNotificationObserver observer,
                                         void *context);
    void set_stream_notification_observer(RpcNotificationObserver observer,
                                          void *context);

    // Device maintenance
    void reset_stats();

    bool recover_can(const char *reason);

    void cancel_requests_from_source(RpcSource source, const char *reason);
    bool background_backpressure_active() const;
    void set_quiesce_mode(bool requested);
    bool quiesce_idle() const;

    // Status snapshots
    const RpcArbiterStats &stats() const { return stats_; }
    RpcRuntimeStatus runtime_status() const;
    const CanDriver &can_driver() const { return can_; }
    uint32_t transport_generation() const {
        return transport_generation_;
    }

private:
    // Request and payload types
    struct PendingRequest {
        bool active = false;
        uint32_t id = 0;
        uint32_t deadline_ms = 0;
        int64_t dispatch_utc_ms = 0;
        RpcSource source = RpcSource::Internal;
        std::string method;
        RpcRequestAdmission admission = RpcRequestAdmission::Normal;
        uint32_t generation = 0;
    };

    struct QueuedRequest {
        uint32_t id = 0;
        uint32_t timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
        RpcSource source = RpcSource::Internal;
        std::string method;
        std::string params_json;
        uint32_t generation = 0;
        RpcRequestAdmission admission = RpcRequestAdmission::Normal;
        RpcDispatchWindow dispatch_window;
    };

    struct RawPassthroughRequest {
        bool active = false;
        uint32_t id = 0;
        uint32_t deadline_ms = 0;
        RpcSource source = RpcSource::Internal;
        StreamCommandType stream_command = StreamCommandType::None;
    };

    struct DeferredPayload {
        enum class Kind : uint8_t {
            Rpc,
            DebugLog,
        };

        Kind kind = Kind::Rpc;
        char *data = nullptr;
        size_t len = 0;

        DeferredPayload() = default;
        ~DeferredPayload();
        DeferredPayload(const DeferredPayload &) = delete;
        DeferredPayload &operator=(const DeferredPayload &) = delete;
        DeferredPayload(DeferredPayload &&other) noexcept;
        DeferredPayload &operator=(DeferredPayload &&other) noexcept;

        bool copy_from(Kind next_kind, const char *payload, size_t payload_len);
        void clear();

    private:
        void move_from(DeferredPayload &other);
    };

    static constexpr size_t RAW_PASSTHROUGH_PENDING_MAX = 8;
    using RequestCompletionQueue =
        FixedQueue<RpcRequestCompletion, AC_RPC_REQUEST_QUEUE_DEPTH>;

    // Event queues
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
    static bool enqueue_datagram_frame(void *context,
                                       const DatagramFrame &frame);

    // Request lifecycle
    void cancel_pending_request(const char *reason);
    void cancel_queued_request(const QueuedRequest &request,
                               const char *reason,
                               RpcCompletionCause cause =
                                   RpcCompletionCause::Cancelled);
    void cancel_all_requests(const char *reason);
    static void complete_request(uint32_t id,
                                 uint32_t generation,
                                 OperationOutcome outcome,
                                 RpcCompletionCause cause,
                                 const std::string *payload,
                                 const char *reason,
                                 bool response_error,
                                 RequestCompletionQueue &completions,
                                 int64_t dispatch_utc_ms = 0,
                                 int64_t response_utc_ms = 0);

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
    bool background_rx_pressure_active(uint32_t now) const;
    bool can_rx_queue_pressure_active() const;
    void note_can_rx_pressure(uint32_t now);
    void note_request_success(RpcSource source, uint32_t now);
    void note_request_timeout(RpcSource source, uint32_t now);
    bool request_allowed_during_quiesce(const QueuedRequest &request) const;

    void dispatch_next_request();
    void check_pending_timeout();
    void process_deferred_payloads(size_t budget);

    // Payload handling
    void handle_event_notification(const char *payload, size_t payload_len);
    void handle_stream_notification(const char *payload, size_t payload_len);
    void note_transport_reset();
    void handle_frame(const RawCanFrame &frame);
    void enqueue_deferred_payload(DeferredPayload::Kind kind,
                                  const char *payload,
                                  size_t payload_len);

    void handle_rpc_payload(const char *payload, size_t payload_len);
    void handle_debug_payload(const char *payload, size_t payload_len);

    std::string format_boot_frame(const RawCanFrame &frame) const;
    const char *source_name(RpcSource source) const;

    // Source-specific event routes
    enum class SourceEventQueue {
        None,
        Report,
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

    // CAN/RPC queues and routes
    CanDriver &can_;
    DatagramRx rpc_rx_{AC_STREAM_FRAME_RAW_MAX};
    DatagramRx log_rx_;
    FixedQueue<DeferredPayload, AC_RPC_PAYLOAD_QUEUE_DEPTH>
        deferred_payloads_;
    FixedQueue<RpcEvent, AC_RPC_EVENT_QUEUE_DEPTH> events_;
    FixedQueue<RpcEvent, AC_REPORT_EVENT_QUEUE_DEPTH> report_events_;
    FixedQueue<RpcEvent, AC_RESMED_OTA_EVENT_QUEUE_DEPTH> resmed_ota_events_;
    SourceEventRoute source_event_routes_[3] = {
        {RpcSource::ResmedOta, SourceEventQueue::ResmedOta, nullptr,
         nullptr},
        {RpcSource::Report, SourceEventQueue::Report, nullptr, nullptr},
        {RpcSource::EdfRecorder, SourceEventQueue::None, nullptr,
         nullptr},
    };

    FixedQueue<QueuedRequest, AC_RPC_REQUEST_QUEUE_DEPTH> requests_;
    RequestCompletionQueue request_completions_;
    PendingRequest pending_;
    RawPassthroughRequest raw_passthrough_[RAW_PASSTHROUGH_PENDING_MAX];
    size_t request_completion_reservations_ = 0;

    // Dispatch state
    QueuedRequest dispatch_retry_;
    bool dispatch_retry_active_ = false;
    uint32_t dispatch_retry_deadline_ms_ = 0;
    uint32_t next_dispatch_retry_ms_ = 0;

    RpcArbiterStats stats_ = {};
    uint32_t next_rpc_id_ = 0;
    uint32_t last_integrated_tx_ms_ = 0;

    // Runtime diagnostics
    uint32_t stats_started_ms_ = 0;
    uint32_t boot_notifications_seen_ = 0;
    uint32_t last_boot_notification_ms_ = 0;
    std::string last_boot_notification_;

    // Notification routing
    StreamBroker &stream_;
    RpcNotificationObserver event_notification_observer_ = nullptr;
    void *event_notification_context_ = nullptr;
    RpcNotificationObserver stream_notification_observer_ = nullptr;
    void *stream_notification_context_ = nullptr;
    uint32_t transport_generation_ = 1;

    // Backpressure and transport admission
    bool raw_rpc_events_enabled_ = false;
    bool quiesce_mode_ = false;

    uint8_t consecutive_scheduler_timeouts_ = 0;
    uint32_t background_backoff_until_ms_ = 0;
    uint32_t background_rx_pressure_until_ms_ = 0;
    uint32_t observed_rx_queue_full_alerts_ = 0;
};

}  // namespace aircannect
