#pragma once

#include <stdint.h>
#include <string>

#include "operation_outcome.h"

namespace aircannect {

struct RpcRequestCommand {
    std::string method;
    std::string params_json;
    uint32_t timeout_ms = 0;
    uint32_t generation = 0;

    bool valid() const {
        return !method.empty() && timeout_ms != 0 && generation != 0;
    }
};

struct RpcRequestCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    std::string payload;
};

class RpcRequestPort {
public:
    virtual ~RpcRequestPort() = default;

    virtual OperationSubmission request(const RpcRequestCommand &command) = 0;
    virtual bool cancel(OperationTicket ticket) = 0;
    virtual bool next_completion(RpcRequestCompletion &completion) = 0;
};

}  // namespace aircannect
