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

struct OperationTicket {
    uint32_t id = 0;
    uint32_t generation = 0;

    constexpr bool valid() const { return id != 0; }

    friend constexpr bool operator==(OperationTicket lhs,
                                     OperationTicket rhs) {
        return lhs.id == rhs.id && lhs.generation == rhs.generation;
    }
    friend constexpr bool operator!=(OperationTicket lhs,
                                     OperationTicket rhs) {
        return !(lhs == rhs);
    }
};

enum class OperationAdmission : uint8_t {
    Accepted,
    Busy,
    Rejected,
};

struct OperationSubmission {
    OperationAdmission admission = OperationAdmission::Rejected;
    OperationTicket ticket;

    static constexpr OperationSubmission accepted(OperationTicket ticket) {
        return {OperationAdmission::Accepted, ticket};
    }
    static constexpr OperationSubmission busy() {
        return {OperationAdmission::Busy, {}};
    }
    static constexpr OperationSubmission rejected() {
        return {OperationAdmission::Rejected, {}};
    }

    constexpr bool accepted() const {
        return admission == OperationAdmission::Accepted && ticket.valid();
    }
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
