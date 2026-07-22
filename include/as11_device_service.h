#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_device_state.h"
#include "rpc_request_port.h"

namespace aircannect {

class As11DeviceService {
public:
    const As11DeviceState &state() const { return state_; }

    bool request_healthcheck(RpcRequestPort &rpc,
                             RpcSource source,
                             uint32_t now_ms);
    bool request_clock_read(RpcRequestPort &rpc,
                            RpcSource source,
                            uint32_t now_ms);
    OperationSubmission request_therapy(RpcRequestPort &rpc,
                                        As11TherapyTarget target,
                                        RpcSource source,
                                        uint32_t now_ms);
    OperationSubmission request_set_datetime_now(RpcRequestPort &rpc,
                                                  RpcSource source,
                                                  uint32_t now_ms,
                                                  int64_t utc_ms);

    bool apply_activity_event_frame(const As11EventFrame &frame,
                                    uint32_t now_ms);
    void device_reset(RpcRequestPort &rpc, uint32_t now_ms);
    void poll(RpcRequestPort &rpc,
              uint32_t now_ms,
              bool background_suspended = false);

    uint32_t revision() const { return revision_; }

private:
    enum class QueryKind : uint8_t {
        None,
        Identity,
        Runtime,
        MotorRuntime,
        Timezone,
        Clock,
        Count,
    };

    struct ScheduledQuery {
        bool scheduled = false;
        uint32_t due_ms = 0;
        RpcSource source = RpcSource::Scheduler;
    };

    static constexpr uint32_t QueryRetryMs = 1000;
    static constexpr size_t QueryCount =
        static_cast<size_t>(QueryKind::Count);

    uint32_t next_generation();
    void note_change();

    void initialize_schedule(uint32_t now_ms);
    void schedule_query(QueryKind kind,
                        uint32_t due_ms,
                        RpcSource source);
    QueryKind next_due_query(uint32_t now_ms,
                             bool background_suspended) const;
    bool submit_query(RpcRequestPort &rpc,
                      QueryKind kind,
                      uint32_t now_ms);
    void complete_query(const RpcRequestCompletion &completion,
                        uint32_t now_ms);
    void complete_therapy(const RpcRequestCompletion &completion,
                          uint32_t now_ms);
    void complete_clock_write(const RpcRequestCompletion &completion);
    void cancel_ticket(RpcRequestPort &rpc, OperationTicket &ticket);

    static size_t query_index(QueryKind kind);
    static bool background_source(RpcSource source);
    static bool completion_succeeded(
        const RpcRequestCompletion &completion);
    static const char *query_params(QueryKind kind);

    As11DeviceState state_;

    ScheduledQuery queries_[QueryCount];
    bool schedule_initialized_ = false;
    QueryKind active_query_kind_ = QueryKind::None;
    OperationTicket query_ticket_;

    OperationTicket therapy_ticket_;
    std::string therapy_method_;

    OperationTicket clock_write_ticket_;

    uint32_t next_generation_ = 0;
    uint32_t revision_ = 0;
};

}  // namespace aircannect
