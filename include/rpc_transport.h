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
#include "rpc_transport_ports.h"

namespace aircannect {

using RpcNotificationObserver = void (*)(void *context,
                                         const char *payload,
                                         size_t payload_len,
                                         uint32_t now_ms);

class RpcTransport final : public RpcRequestPort,
                           public RpcPassthroughPort,
                           public RpcDiagnosticsPort,
                           public RpcQuiescePort {
public:
    explicit RpcTransport(CanDriver &can);

    // Lifecycle and CAN pump
    bool reserve_reassembly_buffers();
    void poll();
    size_t drain_can_rx();

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
    void set_spool_notification_observer(RpcNotificationObserver observer,
                                         void *context);

    // Transport maintenance
    void reset_stats() override;

    bool recover_can(const char *reason) override;

    bool background_backpressure_active() const;
    void set_quiesce_mode(bool requested) override;
    void request_debug_log_rx(bool enabled) override;
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
    void report_framing_error(const char *channel, const std::string &error);
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
                                  uint32_t now);
    bool match_raw_passthrough(uint32_t id,
                               RpcSource &source,
                               uint32_t now);

    bool background_backoff_active(uint32_t now) const;
    bool background_rx_pressure_active(uint32_t now) const;
    bool can_rx_queue_pressure_active() const;
    void note_can_rx_pressure(uint32_t now);
    void note_request_success(RpcSource source, uint32_t now);
    void note_request_timeout(RpcSource source, uint32_t now);
    bool request_allowed_during_quiesce(const QueuedRequest &request) const;
    bool quiesce_idle() const;
    void poll_debug_log_rx_filter();

    void dispatch_next_request();
    void check_pending_timeout();
    void process_deferred_payloads(size_t budget);

    // Payload handling
    void handle_event_notification(const char *payload, size_t payload_len);
    void handle_stream_notification(const char *payload, size_t payload_len);
    void handle_spool_notification(const char *payload, size_t payload_len);
    void note_transport_reset();
    void handle_frame(const RawCanFrame &frame);
    void enqueue_deferred_payload(DeferredPayload::Kind kind,
                                  const char *payload,
                                  size_t payload_len);

    void handle_rpc_payload(const char *payload, size_t payload_len);
    void handle_debug_payload(const char *payload, size_t payload_len);

    std::string format_boot_frame(const RawCanFrame &frame) const;
    const char *source_name(RpcSource source) const;

    // CAN/RPC queues
    CanDriver &can_;
    DatagramRx rpc_rx_{AC_STREAM_FRAME_RAW_MAX};
    DatagramRx log_rx_;
    FixedQueue<DeferredPayload, AC_RPC_PAYLOAD_QUEUE_DEPTH>
        deferred_payloads_;
    FixedQueue<RpcEvent, AC_RPC_EVENT_QUEUE_DEPTH> events_;
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
    RpcNotificationObserver spool_notification_observer_ = nullptr;
    void *spool_notification_context_ = nullptr;
    uint32_t transport_generation_ = 1;

    // Backpressure and transport admission
    bool raw_rpc_forwarding_enabled_ = false;
    bool quiesce_mode_ = false;
    bool debug_log_rx_requested_ = true;

    uint8_t consecutive_scheduler_timeouts_ = 0;
    uint32_t background_backoff_until_ms_ = 0;
    uint32_t background_rx_pressure_until_ms_ = 0;
    uint32_t observed_rx_queue_full_alerts_ = 0;
};

}  // namespace aircannect
