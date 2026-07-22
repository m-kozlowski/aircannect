#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "report_spool_types.h"
#include "rpc_request_port.h"
#include "spool_client_status.h"

namespace aircannect {

static constexpr uint32_t AC_SPOOL_CLIENT_RPC_TIMEOUT_MS = 8000;
static constexpr uint32_t AC_SPOOL_CLIENT_PULL_RPC_TIMEOUT_MS = 30000;
static constexpr uint32_t AC_SPOOL_CLIENT_FRAGMENT_TIMEOUT_MS = 30000;
static constexpr uint8_t AC_SPOOL_CLIENT_ROUND_RETRIES = 2;

struct SpoolClientRequest {
    std::string spool_type;
    std::string from_dt;
    size_t max_size = 65536;
    size_t fragment_max = 2808;
    size_t max_notifications = 0;
    uint16_t max_rounds = 64;
    bool pace_on_backpressure = false;
    bool stream_rounds = false;
};

class SpoolClient {
public:
    explicit SpoolClient(RpcRequestPort &rpc) : rpc_(rpc) {}
    ~SpoolClient();

    bool begin(const SpoolClientRequest &request);
    void poll(bool background_backpressure_active);
    bool handle_spool_notification(const char *payload, size_t payload_len);
    void note_notification_loss(const char *reason);
    void reset();

    bool active() const;
    bool complete() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Failed; }
    const SpoolClientStatus &status() const { return status_; }
    bool take_completed_round(ReportSpoolResult &out);
    void move_result_to(ReportSpoolResult &out);

private:
    struct FragmentSlice {
        uint32_t seq = 0;
        size_t offset = 0;
        size_t len = 0;
    };

    enum class State : uint8_t {
        Idle,
        WaitStart,
        WaitPull,
        WaitFragments,
        RoundReady,
        Done,
        Failed,
    };

    enum class PendingSubmit : uint8_t {
        None,
        Start,
        Pull,
    };

    void schedule_start();
    void schedule_pull();
    bool submit_pending();
    bool submit_start();
    bool submit_pull();
    void poll_rpc_completion();
    void cancel_rpc_request();
    bool handle_start_response(const std::string &payload);
    bool handle_pull_response(const std::string &payload);
    bool append_base64_fragment(const char *data, size_t len, uint32_t seq);
    bool ensure_round_fragment_capacity(size_t count);
    void clear_round_fragments(bool release_storage);
    bool normalize_current_round();
    bool compute_hash(size_t offset, size_t len);
    bool capture_completed_round();
    void continue_after_completed_round();
    bool retry_current_round(const char *reason);
    void finish();
    void fail(const char *message);
    void update_status(uint32_t now_ms);
    std::string build_start_params() const;
    std::string build_pull_params() const;

    RpcRequestPort &rpc_;
    SpoolClientRequest request_;
    SpoolClientStatus status_;
    ReportSpoolResult result_;
    State state_ = State::Idle;
    PendingSubmit pending_submit_ = PendingSubmit::None;
    PendingSubmit submitted_ = PendingSubmit::None;
    OperationTicket rpc_ticket_;
    uint32_t rpc_generation_ = 0;
    uint32_t active_spool_id_ = 0;
    uint32_t state_started_ms_ = 0;
    uint32_t fetch_started_ms_ = 0;
    uint32_t next_pull_submit_ms_ = 0;
    std::string next_spool_address_json_;
    FragmentSlice *round_fragments_ = nullptr;
    size_t round_fragment_capacity_ = 0;
    size_t round_fragment_count_ = 0;
    size_t round_start_offset_ = 0;
    uint32_t round_start_fragment_count_ = 0;
    uint8_t round_retry_count_ = 0;
    bool completed_round_ready_ = false;
    ReportSpoolResult completed_round_;
};

}  // namespace aircannect
