#pragma once

#include <stdint.h>
#include <string>

#include "as11_settings.h"
#include "rpc_request_port.h"

namespace aircannect {

class As11SettingsManager {
public:
    const As11SettingsState &state() const { return state_; }

    bool request_refresh(RpcRequestPort &rpc,
                         RpcSource source,
                         uint32_t now_ms);
    OperationSubmission write(RpcRequestPort &rpc,
                              const std::string &params_json,
                              RpcSource source,
                              uint32_t now_ms);

    void invalidate(RpcRequestPort &rpc,
                    RpcSource source,
                    uint32_t now_ms);
    void device_reset(RpcRequestPort &rpc);
    void poll(RpcRequestPort &rpc, uint32_t now_ms, bool suspended = false);

    bool refresh_pending() const;
    uint32_t revision() const { return revision_; }

private:
    static constexpr uint32_t RefreshRetryMs = 1000;
    static constexpr uint32_t ReadbackTimeoutMs = 20000;

    uint32_t next_generation();
    void note_change();

    bool submit_refresh(RpcRequestPort &rpc, RpcSource source);
    void schedule_refresh(RpcSource source,
                          uint32_t now_ms,
                          uint32_t delay_ms = RefreshRetryMs);
    void complete_write(RpcRequestPort &rpc,
                        const RpcRequestCompletion &completion,
                        uint32_t now_ms);
    void complete_refresh(const RpcRequestCompletion &completion,
                          uint32_t now_ms);

    static bool background_source(RpcSource source);
    static bool completion_succeeded(const RpcRequestCompletion &completion);

    As11SettingsState state_;

    OperationTicket write_ticket_;
    RpcSource write_source_ = RpcSource::Internal;

    OperationTicket refresh_ticket_;
    RpcSource refresh_source_ = RpcSource::Scheduler;
    bool refresh_retry_pending_ = false;
    bool refresh_again_pending_ = false;
    uint32_t next_refresh_retry_ms_ = 0;

    uint32_t next_generation_ = 0;
    uint32_t revision_ = 0;
};

}  // namespace aircannect
