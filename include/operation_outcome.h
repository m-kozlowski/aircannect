#pragma once

#include <stdint.h>

namespace aircannect {

enum class OperationDisposition : uint8_t {
    Succeeded,
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

    static constexpr OperationOutcome succeeded() {
        return {OperationDisposition::Succeeded};
    }
    static constexpr OperationOutcome retry() {
        return {OperationDisposition::Retry};
    }
    static constexpr OperationOutcome failed() {
        return {OperationDisposition::Failed};
    }
    static constexpr OperationOutcome cancelled() {
        return {OperationDisposition::Cancelled};
    }

    constexpr bool complete() const {
        return disposition == OperationDisposition::Succeeded ||
               disposition == OperationDisposition::Failed ||
               disposition == OperationDisposition::Cancelled;
    }
    constexpr bool should_retry() const {
        return disposition == OperationDisposition::Retry;
    }
};

}  // namespace aircannect
