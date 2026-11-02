#include "report_request_queue.h"

#include <algorithm>

namespace aircannect {

ReportRequestQueue::ReportRequestQueue(ReportArtifactRequest *slots,
                                       size_t capacity) :
    slots_(slots), capacity_(slots ? capacity : 0) {}

bool ReportRequestQueue::same_artifact_identity(
    const ReportArtifactKey &lhs,
    const ReportArtifactKey &rhs) {
    return lhs.sleep_day == rhs.sleep_day;
}

bool ReportRequestQueue::ready(const ReportArtifactRequest &request,
                               uint32_t now_ms) {
    return request.ready_at_ms == 0 ||
           static_cast<int32_t>(now_ms - request.ready_at_ms) >= 0;
}

size_t ReportRequestQueue::find_artifact(
    const ReportArtifactKey &artifact,
    ReportRequestIntent intent) const {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].intent == intent &&
            same_artifact_identity(slots_[i].artifact, artifact)) return i;
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

size_t ReportRequestQueue::find_evictable(
    ReportRequestPriority priority) const {
    for (size_t i = count_; i > 0; --i) {
        if (report_request_priority_higher(priority, slots_[i - 1].priority)) {
            return i - 1;
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
    uint32_t generation,
    bool force_rebuild,
    ReportRequestIntent intent) {
    if (!artifact.valid() || generation == 0 || !slots_ ||
        (intent == ReportRequestIntent::LoadMetadata && force_rebuild)) {
        return {};
    }

    bool replaced = false;
    size_t existing = find_artifact(artifact, intent);
    const size_t other = find_artifact(artifact, intent == ReportRequestIntent::Build
        ? ReportRequestIntent::LoadMetadata : ReportRequestIntent::Build);

    if (other < count_) {
        if (intent == ReportRequestIntent::LoadMetadata) {
            // A delayed build keeps its backoff; an independent read can run now.
            if (slots_[other].ready_at_ms == 0 && slots_[other].artifact == artifact) {
                return {ReportRequestEnqueueStatus::AlreadyQueued, slots_[other].ticket};
            }
        } else if (existing == count_) {
            existing = other;
        } else {
            erase(other);
            existing = find_artifact(artifact, intent);
        }
    }

    if (existing < count_) {
        if (slots_[existing].artifact == artifact) {

            ReportArtifactRequest &request = slots_[existing];
            const bool priority_upgrade = report_request_priority_higher(
                priority, request.priority);
            const bool rebuild_upgrade =
                force_rebuild && !request.force_rebuild;
            const bool intent_upgrade = intent == ReportRequestIntent::Build &&
                request.intent == ReportRequestIntent::LoadMetadata;

            if (priority_upgrade || rebuild_upgrade || intent_upgrade) {
                request.ticket = next_ticket(generation);
                if (priority_upgrade) request.priority = priority;
                request.force_rebuild =
                    request.force_rebuild || force_rebuild;
                if (intent_upgrade) request.intent = intent;
                request.ready_at_ms = 0;
                request.attempts = 0;
                return {ReportRequestEnqueueStatus::Replaced,
                        request.ticket};
            }
            return {ReportRequestEnqueueStatus::AlreadyQueued,
                    slots_[existing].ticket};
        }

        erase(existing);
        replaced = true;
    }

    if (count_ >= capacity_) {
        const size_t evictable = find_evictable(priority);
        if (evictable == count_ ||
            (intent == ReportRequestIntent::LoadMetadata &&
             slots_[evictable].intent == ReportRequestIntent::Build &&
             same_artifact_identity(slots_[evictable].artifact, artifact))) {
            return {ReportRequestEnqueueStatus::Full, {}};
        }

        erase(evictable);
        replaced = true;
    }

    ReportArtifactRequest &request = slots_[count_++];
    request.artifact = artifact;
    request.ticket = next_ticket(generation);
    request.priority = priority;
    request.force_rebuild = force_rebuild;
    request.intent = intent;
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
        index = find_ready(ReportRequestPriority::Reconcile, now_ms);
    }
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

    if (find_artifact(request.artifact, request.intent) < count_) {
        return OperationOutcome::cancelled();
    }

    request.attempts++;
    request.ready_at_ms = delay_ms == 0 ? 0 : now_ms + delay_ms;
    if (delay_ms != 0 && request.ready_at_ms == 0) {
        request.ready_at_ms = 1;
    }
    slots_[count_++] = request;
    return OperationOutcome::retry();
}

size_t ReportRequestQueue::cancel_background() {
    size_t removed = 0;
    for (size_t i = 0; i < count_;) {
        if (slots_[i].priority == ReportRequestPriority::Foreground) {
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

bool ReportRequestQueue::contains(ReportRequestPriority priority) const {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].priority == priority) return true;
    }
    return false;
}

}  // namespace aircannect
