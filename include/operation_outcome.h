#pragma once

#include <stdint.h>

namespace aircannect {

enum class OperationDisposition : uint8_t {
    Succeeded,
    Deferred,
    Retry,
    Failed,
    Cancelled,
};

struct OperationOutcome {
    OperationDisposition disposition = OperationDisposition::Failed;
    uint32_t retry_after_ms = 0;

    static constexpr OperationOutcome succeeded() {
        return {OperationDisposition::Succeeded, 0};
    }
    static constexpr OperationOutcome deferred(uint32_t retry_after_ms) {
        return {OperationDisposition::Deferred, retry_after_ms};
    }
    static constexpr OperationOutcome retry(uint32_t retry_after_ms) {
        return {OperationDisposition::Retry, retry_after_ms};
    }
    static constexpr OperationOutcome failed() {
        return {OperationDisposition::Failed, 0};
    }
    static constexpr OperationOutcome cancelled() {
        return {OperationDisposition::Cancelled, 0};
    }

    constexpr bool complete() const {
        return disposition == OperationDisposition::Succeeded ||
               disposition == OperationDisposition::Failed ||
               disposition == OperationDisposition::Cancelled;
    }
    constexpr bool should_retry() const {
        return disposition == OperationDisposition::Deferred ||
               disposition == OperationDisposition::Retry;
    }
};

}  // namespace aircannect
