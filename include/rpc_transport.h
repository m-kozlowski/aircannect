#pragma once

#include <Arduino.h>
#include <memory>
#include <stdint.h>
#include <string>

#include "board.h"
#include "fixed_queue.h"
#include "rpc_application_link.h"
#include "rpc_request_port.h"
#include "rpc_transport_ports.h"

namespace aircannect {

using RpcNotificationObserver = void (*)(void *context,
                                         RpcPayloadView payload,
                                         uint32_t now_ms);
using RpcRetainedNotificationObserver = void (*)(
    void *context,
    const RpcPayloadRef &payload,
    uint32_t now_ms);
class RpcTransport final : public RpcRequestPort,
                           public RpcPassthroughPort,
                           public RpcDiagnosticsPort,
                           public RpcQuiescePort {
public:
    explicit RpcTransport(RpcApplicationLink &link);

    // Lifecycle and link pump
    bool reserve_reassembly_buffers();
    void poll();

    // RPC submission
    bool submit_raw_payload(const std::string &payload,
                            RpcSource source) override;

    bool send_request(const std::string &method,
                      const std::string &params_json,
                      RpcSource source,
                      uint32_t timeout_ms = 0) override;
    OperationSubmission request(const RpcRequestCommand &command) override;
    bool cancel(OperationTicket ticket) override;
    bool take_completion(OperationTicket ticket,
                         RpcRequestCompletion &completion) override;

    // Event routing
    bool next_event(RpcEvent &event);

    void set_raw_rpc_forwarding_enabled(bool enabled);
    void set_event_notification_observer(RpcNotificationObserver observer,
                                         void *context);
    void set_stream_notification_observer(RpcNotificationObserver observer,
                                          void *context);
    void set_spool_notification_observer(
        RpcRetainedNotificationObserver observer,
        void *context);

    void accept_debug_payload(const RpcPayloadRef &payload);
    void accept_debug_framing_error(const char *detail);
    void accept_boot_notification(const char *detail);
    void accept_link_reset(const char *reason);

    // Transport maintenance
    void reset_stats() override;

    bool background_backpressure_active() const;
    void set_as11_unavailable(bool unavailable);
    void set_quiesce_mode(bool requested) override;
    bool send_quiesce_request(const std::string &method,
                              const std::string &params_json) override;
    RpcQuiesceStatus quiesce_status() const override;

    // Status snapshots
    RpcTransportStats stats() const override { return stats_; }
    RpcTransportStatus runtime_status() const override;
    uint32_t transport_generation() const {
        return transport_generation_;
    }

private:
    // Request and payload types
    struct PendingRequest {
        bool active = false;
        uint32_t id = 0;
        uint32_t deadline_ms = 0;
        uint32_t dispatch_ms = 0;
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
    };

    struct RetiredAddressedRequest {
        uint32_t id = 0;
        uint32_t retired_ms = 0;
        RpcCompletionCause cause = RpcCompletionCause::Cancelled;
    };

    struct DeferredPayload {
        enum class Kind : uint8_t {
            Rpc,
            DebugLog,
        };

        Kind kind = Kind::Rpc;
        RpcPayloadRef payload;
    };

    static constexpr size_t RAW_PASSTHROUGH_PENDING_MAX = 8;
    static constexpr size_t RETIRED_ADDRESSED_REQUEST_MAX = 8;
    static constexpr uint32_t RETIRED_ADDRESSED_REQUEST_TTL_MS = 120000;
    using RequestCompletionQueue =
        FixedQueue<RpcRequestCompletion, AC_RPC_REQUEST_QUEUE_DEPTH>;

    // Event queues
    void push_event(RpcEventKind kind,
                    RpcPayloadRef payload,
                    RpcSource source = RpcSource::Internal,
                    uint32_t id = 0);
    void push_text_event(RpcEventKind kind,
                         const char *payload,
                         size_t payload_len,
                         RpcSource source = RpcSource::Internal,
                         uint32_t id = 0);
    void publish_framing_error(const char *channel, const std::string &error);
    bool enqueue_request(QueuedRequest &request);
    RpcLinkSendResult send_payload(const std::string &payload);

    // Request lifecycle
    void cancel_pending_request(const char *reason);
    void cancel_queued_request(const QueuedRequest &request,
                               const char *reason,
                               RpcCompletionCause cause =
                                   RpcCompletionCause::Cancelled);
    void cancel_all_requests(const char *reason);
    void remember_retired_addressed_request(
        const PendingRequest &request,
        RpcCompletionCause cause,
        const char *reason);
    bool consume_retired_addressed_response(uint32_t id, uint32_t now_ms);
    static void complete_request(uint32_t id,
                                 uint32_t generation,
                                 OperationOutcome outcome,
                                 RpcCompletionCause cause,
                                 const RpcPayloadRef &payload,
                                 const char *reason,
                                 bool response_error,
                                 RequestCompletionQueue &completions,
                                 int64_t dispatch_utc_ms = 0,
                                 int64_t response_utc_ms = 0,
                                 uint32_t dispatch_ms = 0,
                                 uint32_t response_ms = 0);

    void expire_raw_passthrough(uint32_t now);
    void remember_raw_passthrough(uint32_t id,
                                  RpcSource source,
                                  uint32_t now);
    bool match_raw_passthrough(uint32_t id,
                               RpcSource &source,
                               uint32_t now);

    bool background_rx_pressure_active(uint32_t now) const;
    void note_link_rx_pressure(uint32_t now);
    bool request_allowed_during_quiesce(const QueuedRequest &request) const;
    bool request_allowed_while_unavailable(
        const QueuedRequest &request) const;
    void cancel_requests_while_unavailable();
    bool quiesce_idle() const;
    void dispatch_next_request();
    void check_pending_timeout();
    void process_deferred_payloads(size_t budget);
    void process_link_events(size_t budget);

    // Payload handling
    void handle_event_notification(const RpcPayloadRef &payload);
    void handle_stream_notification(const RpcPayloadRef &payload);
    void handle_spool_notification(const RpcPayloadRef &payload);
    void note_transport_reset();
    void enqueue_deferred_payload(DeferredPayload::Kind kind,
                                  RpcPayloadView payload);
    void enqueue_deferred_payload(DeferredPayload::Kind kind,
                                  const RpcPayloadRef &payload);

    void handle_rpc_payload(const RpcPayloadRef &payload);
    void handle_debug_payload(const RpcPayloadRef &payload);

    const char *source_name(RpcSource source) const;

    // Application link and RPC queues
    RpcApplicationLink &link_;
    FixedQueue<DeferredPayload, AC_RPC_PAYLOAD_QUEUE_DEPTH>
        deferred_payloads_;
    FixedQueue<RpcEvent, AC_RPC_EVENT_QUEUE_DEPTH> events_;
    FixedQueue<QueuedRequest, AC_RPC_REQUEST_QUEUE_DEPTH> requests_;
    RequestCompletionQueue request_completions_;
    PendingRequest pending_;
    RawPassthroughRequest raw_passthrough_[RAW_PASSTHROUGH_PENDING_MAX];
    RetiredAddressedRequest
        retired_addressed_requests_[RETIRED_ADDRESSED_REQUEST_MAX];
    size_t next_retired_addressed_request_ = 0;
    size_t request_completion_reservations_ = 0;

    // Dispatch state
    QueuedRequest dispatch_retry_;
    bool dispatch_retry_active_ = false;
    uint32_t dispatch_retry_deadline_ms_ = 0;
    uint32_t next_dispatch_retry_ms_ = 0;

    RpcTransportStats stats_ = {};
    uint32_t next_rpc_id_ = 0;
    uint32_t last_integrated_tx_ms_ = 0;

    // Runtime diagnostics
    uint32_t stats_started_ms_ = 0;
    uint32_t boot_notifications_seen_ = 0;
    uint32_t last_boot_notification_ms_ = 0;
    std::string last_boot_notification_;

    // Notification routing
    RpcNotificationObserver event_notification_observer_ = nullptr;
    void *event_notification_context_ = nullptr;
    RpcNotificationObserver stream_notification_observer_ = nullptr;
    void *stream_notification_context_ = nullptr;
    RpcRetainedNotificationObserver spool_notification_observer_ = nullptr;
    void *spool_notification_context_ = nullptr;
    uint32_t transport_generation_ = 1;

    // Backpressure and transport admission
    bool raw_rpc_forwarding_enabled_ = false;
    bool quiesce_mode_ = false;
    bool as11_unavailable_ = false;

    uint32_t background_rx_pressure_until_ms_ = 0;
    uint32_t observed_rx_pressure_events_ = 0;
};

}  // namespace aircannect
