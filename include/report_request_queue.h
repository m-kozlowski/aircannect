#pragma once

#include <stddef.h>
#include <stdint.h>

#include "operation_outcome.h"
#include "report_artifact_key.h"

namespace aircannect {

enum class ReportRequestPriority : uint8_t {
    Foreground,
    Reconcile,
    Idle,
};

constexpr bool report_request_priority_higher(ReportRequestPriority lhs,
                                              ReportRequestPriority rhs) {
    return static_cast<uint8_t>(lhs) < static_cast<uint8_t>(rhs);
}

struct ReportArtifactRequest {
    ReportArtifactKey artifact;
    OperationTicket ticket;
    ReportRequestPriority priority = ReportRequestPriority::Foreground;
    uint32_t ready_at_ms = 0;
    uint8_t attempts = 0;
};

enum class ReportRequestEnqueueStatus : uint8_t {
    Queued,
    AlreadyQueued,
    Replaced,
    Full,
    Invalid,
};

struct ReportRequestEnqueueResult {
    ReportRequestEnqueueStatus status = ReportRequestEnqueueStatus::Invalid;
    OperationTicket ticket;

    bool queued() const {
        return status == ReportRequestEnqueueStatus::Queued ||
               status == ReportRequestEnqueueStatus::Replaced;
    }
};

enum class ReportRequestSelection : uint8_t {
    Empty,
    Waiting,
    Ready,
};

class ReportRequestQueue {
public:
    ReportRequestQueue(ReportArtifactRequest *slots, size_t capacity);

    ReportRequestEnqueueResult enqueue(const ReportArtifactKey &artifact,
                                       ReportRequestPriority priority,
                                       uint32_t generation);
    ReportRequestSelection take_next(uint32_t now_ms,
                                     ReportArtifactRequest &request);
    OperationOutcome retry(ReportArtifactRequest request,
                           uint32_t now_ms,
                           uint32_t delay_ms,
                           uint8_t max_attempts);

    size_t cancel_generation(uint32_t generation);
    void clear();

    size_t size() const { return count_; }
    size_t capacity() const { return capacity_; }

private:
    static bool same_artifact_identity(const ReportArtifactKey &lhs,
                                       const ReportArtifactKey &rhs);
    static bool ready(const ReportArtifactRequest &request, uint32_t now_ms);

    size_t find_artifact(const ReportArtifactKey &artifact) const;
    size_t find_ready(ReportRequestPriority priority, uint32_t now_ms) const;
    size_t find_evictable(ReportRequestPriority priority) const;
    void erase(size_t index);
    OperationTicket next_ticket(uint32_t generation);

    ReportArtifactRequest *slots_ = nullptr;
    size_t capacity_ = 0;
    size_t count_ = 0;
    uint32_t next_ticket_id_ = 1;
};

}  // namespace aircannect
