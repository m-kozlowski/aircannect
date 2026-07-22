#include "report_request_queue.h"

namespace aircannect {

ReportRequestQueue::ReportRequestQueue(ReportArtifactRequest *slots,
                                       size_t capacity) :
    slots_(slots), capacity_(slots ? capacity : 0) {}

bool ReportRequestQueue::same_artifact_identity(
    const ReportArtifactKey &lhs,
    const ReportArtifactKey &rhs) {
    return lhs.sleep_day == rhs.sleep_day && lhs.kind == rhs.kind &&
           lhs.range_start_ms == rhs.range_start_ms &&
           lhs.range_end_ms == rhs.range_end_ms;
}

bool ReportRequestQueue::ready(const ReportArtifactRequest &request,
                               uint32_t now_ms) {
    return request.ready_at_ms == 0 ||
           static_cast<int32_t>(now_ms - request.ready_at_ms) >= 0;
}

size_t ReportRequestQueue::find_artifact(
    const ReportArtifactKey &artifact) const {
    for (size_t i = 0; i < count_; ++i) {
        if (same_artifact_identity(slots_[i].artifact, artifact)) return i;
    }
    return count_;
}

size_t ReportRequestQueue::find_ready(ReportRequestPriority priority,
                                      uint32_t now_ms) const {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].priority == priority && ready(slots_[i], now_ms)) {
            return i;
        }
    }
    return count_;
}

void ReportRequestQueue::erase(size_t index) {
    if (index >= count_) return;

    for (size_t i = index + 1; i < count_; ++i) {
        slots_[i - 1] = slots_[i];
    }
    slots_[--count_] = {};
}

OperationTicket ReportRequestQueue::next_ticket(uint32_t generation) {
    uint32_t id = next_ticket_id_++;
    if (id == 0) id = next_ticket_id_++;
    return {id, generation};
}

ReportRequestEnqueueResult ReportRequestQueue::enqueue(
    const ReportArtifactKey &artifact,
    ReportRequestPriority priority,
    uint32_t generation) {
    if (!artifact.valid() || generation == 0 || !slots_) return {};

    bool replaced = false;
    const size_t existing = find_artifact(artifact);
    if (existing < count_) {
        if (slots_[existing].artifact == artifact &&
            slots_[existing].ticket.generation == generation) {
            if (priority == ReportRequestPriority::Foreground) {
                slots_[existing].priority = priority;
            }
            return {ReportRequestEnqueueStatus::AlreadyQueued,
                    slots_[existing].ticket};
        }

        erase(existing);
        replaced = true;
    }

    if (count_ >= capacity_) {
        return {ReportRequestEnqueueStatus::Full, {}};
    }

    ReportArtifactRequest &request = slots_[count_++];
    request.artifact = artifact;
    request.ticket = next_ticket(generation);
    request.priority = priority;

    return {replaced ? ReportRequestEnqueueStatus::Replaced
                     : ReportRequestEnqueueStatus::Queued,
            request.ticket};
}

ReportRequestSelection ReportRequestQueue::take_next(
    uint32_t now_ms,
    ReportArtifactRequest &request) {
    if (count_ == 0) return ReportRequestSelection::Empty;

    size_t index = find_ready(ReportRequestPriority::Foreground, now_ms);
    if (index == count_) {
        index = find_ready(ReportRequestPriority::Idle, now_ms);
    }
    if (index == count_) return ReportRequestSelection::Waiting;

    request = slots_[index];
    erase(index);
    return ReportRequestSelection::Ready;
}

OperationOutcome ReportRequestQueue::retry(ReportArtifactRequest request,
                                           uint32_t now_ms,
                                           uint32_t delay_ms,
                                           uint8_t max_attempts) {
    if (!request.artifact.valid() || !request.ticket.valid()) {
        return OperationOutcome::failed();
    }
    if (request.attempts >= max_attempts || count_ >= capacity_) {
        return OperationOutcome::failed();
    }

    if (find_artifact(request.artifact) < count_) {
        return OperationOutcome::cancelled();
    }

    request.attempts++;
    request.ready_at_ms = delay_ms == 0 ? 0 : now_ms + delay_ms;
    if (delay_ms != 0 && request.ready_at_ms == 0) {
        request.ready_at_ms = 1;
    }
    slots_[count_++] = request;
    return OperationOutcome::retry(delay_ms);
}

size_t ReportRequestQueue::cancel_generation(uint32_t generation) {
    size_t removed = 0;
    for (size_t i = 0; i < count_;) {
        if (slots_[i].ticket.generation != generation) {
            ++i;
            continue;
        }

        erase(i);
        removed++;
    }
    return removed;
}

void ReportRequestQueue::clear() {
    while (count_ > 0) erase(count_ - 1);
}

}  // namespace aircannect
