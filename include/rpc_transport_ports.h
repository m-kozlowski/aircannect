#pragma once

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>

#include "rpc_request_port.h"

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

inline RpcPayloadRef make_rpc_payload_ref(std::string payload) {
    return std::make_shared<const std::string>(std::move(payload));
}

struct RpcEvent {
    RpcEventKind kind = RpcEventKind::Info;
    RpcSource source = RpcSource::Internal;
    uint32_t id = 0;
    RpcPayloadRef payload;

    const char *payload_c_str() const {
        return payload ? payload->c_str() : "";
    }
};

struct RpcTransportStats {
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

struct RpcTransportStatus {
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

struct RpcQuiesceStatus {
    bool idle = false;
    bool pending_request = false;
    bool dispatch_retry = false;
    bool debug_log_rx_enabled = true;
    bool debug_log_filter_pending = false;
    size_t request_queue_depth = 0;
    size_t payload_queue_depth = 0;
    size_t tx_queue_depth = 0;
};

class RpcPassthroughPort {
public:
    virtual ~RpcPassthroughPort() = default;

    virtual bool submit_raw_payload(const std::string &payload,
                                    RpcSource source) = 0;
    virtual bool send_request(const std::string &method,
                              const std::string &params_json,
                              RpcSource source,
                              uint32_t timeout_ms = 0) = 0;
};

class RpcDiagnosticsPort {
public:
    virtual ~RpcDiagnosticsPort() = default;

    virtual RpcTransportStatus runtime_status() const = 0;
    virtual RpcTransportStats stats() const = 0;
    virtual void reset_stats() = 0;
    virtual bool recover_can(const char *reason) = 0;
};

class RpcQuiescePort {
public:
    virtual ~RpcQuiescePort() = default;

    virtual void set_quiesce_mode(bool requested) = 0;
    virtual void request_debug_log_rx(bool enabled) = 0;
    virtual RpcQuiesceStatus quiesce_status() const = 0;
};

}  // namespace aircannect
