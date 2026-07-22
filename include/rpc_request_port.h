#pragma once

#include <stdint.h>
#include <string>

#include "operation_outcome.h"

namespace aircannect {

enum class RpcSource : uint8_t {
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

struct RpcRequestCommand {
    std::string method;
    std::string params_json;
    RpcSource source = RpcSource::Internal;
    uint32_t timeout_ms = 0;
    uint32_t generation = 0;

    bool valid() const {
        return !method.empty() && timeout_ms != 0 && generation != 0;
    }
};

enum class RpcCompletionCause : uint8_t {
    Response,
    Timeout,
    Cancelled,
    DispatchFailure,
};

struct RpcRequestCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    RpcCompletionCause cause = RpcCompletionCause::Cancelled;
    std::string payload;
    std::string reason;
    bool response_error = false;
};

class RpcRequestPort {
public:
    virtual ~RpcRequestPort() = default;

    virtual OperationSubmission request(const RpcRequestCommand &command) = 0;
    virtual bool cancel(OperationTicket ticket) = 0;
    virtual bool take_completion(OperationTicket ticket,
                                 RpcRequestCompletion &completion) = 0;
};

}  // namespace aircannect
