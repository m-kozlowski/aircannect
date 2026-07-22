#include "report_engine.h"

#include <utility>

#include "report_sources.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t PLAN_RETRY_DELAY_MS = 50;
constexpr uint8_t PLAN_RETRY_LIMIT = 2;

}  // namespace

ReportEngine::ReportEngine(ReportArtifactRequest *queue_slots,
                           size_t queue_capacity) :
    queue_(queue_slots, queue_capacity) {}

void ReportEngine::begin(StorageReadPort &read_port,
                         StorageAtomicWritePort &write_port,
                         ReportArtifactAssembler &assembler) {
    read_port_ = &read_port;
    assembler_ = &assembler;
    executor_.begin(read_port);
    artifact_store_.begin(write_port);
}

void ReportEngine::publish_catalog(std::shared_ptr<const NightCatalog> catalog) {
    catalog_ = std::move(catalog);

    if (published_) {
        const NightCatalogRecord *published_night =
            catalog_ ? catalog_->find(published_->key.sleep_day) : nullptr;
        if (!published_night ||
            published_night->source_revision != published_->key.source_revision) {
            published_.reset();
        }
    }

    if (!active_ || !catalog_) return;

    const NightCatalogRecord *night =
        catalog_->find(active_request_.artifact.sleep_day);
    if (!night ||
        night->source_revision != active_request_.artifact.source_revision) {
        cancel_active_work();
    }
}

ReportRequestEnqueueResult ReportEngine::request(
    const ReportArtifactKey &artifact,
    ReportRequestPriority priority,
    uint32_t generation) {
    const ReportArtifactKey canonical = build_key(artifact);
    if (!canonical.valid() || generation == 0) return {};

    if (active_ && same_build(active_request_.artifact, canonical)) {
        if (active_request_.artifact == canonical &&
            active_request_.ticket.generation == generation) {
            return {ReportRequestEnqueueStatus::AlreadyQueued,
                    active_request_.ticket};
        }
        cancel_active_work();
    }

    return queue_.enqueue(canonical, priority, generation);
}

size_t ReportEngine::cancel_generation(uint32_t generation) {
    size_t cancelled = queue_.cancel_generation(generation);
    if (active_ && active_request_.ticket.generation == generation) {
        cancel_active_work();
        ++cancelled;
    }
    return cancelled;
}

void ReportEngine::clear() {
    queue_.clear();
    if (active_) {
        cancel_active_work();
        if (assembler_ && !publishing_) assembler_->discard_build();
    }
    reset_active();
    published_.reset();
    last_completion_ = {};
}

bool ReportEngine::poll(uint32_t now_ms, size_t record_budget) {
    if (!read_port_ || !assembler_) return false;

    bool worked = false;
    if (!active_) {
        if (queue_.size() == 0) return false;
        if (!catalog_) return false;
        worked = start_next(now_ms);
    }

    if (!active_) return worked;

    if (publishing_) {
        worked = artifact_store_.poll() || worked;
        if (artifact_store_.status().terminal()) {
            worked = finish_publication() || worked;
        }
    } else {
        worked = executor_.poll(record_budget) || worked;
        if (executor_.status().terminal()) {
            worked = finish_execution(now_ms) || worked;
        }
    }
    return worked;
}

ReportEngineStatus ReportEngine::status() const {
    ReportEngineStatus out;
    out.queued = queue_.size();
    out.active_request = active_request_;
    out.executor = executor_.status();
    out.store = artifact_store_.status();
    out.last_completion = last_completion_;

    if (active_ && publishing_) {
        out.state = ReportEngineState::Publishing;
    } else if (active_) {
        out.state = ReportEngineState::Executing;
    } else if (queue_.size() > 0 && !catalog_) {
        out.state = ReportEngineState::WaitingForCatalog;
    } else if (queue_.size() > 0) {
        out.state = ReportEngineState::Queued;
    }
    return out;
}

std::shared_ptr<const ReportArtifactBundle> ReportEngine::take_published() {
    return std::move(published_);
}

ReportArtifactKey ReportEngine::build_key(
    const ReportArtifactKey &artifact) {
    if (artifact.kind != ReportArtifactKind::Overview) return artifact;
    return ReportArtifactKey::result(artifact.sleep_day,
                                     artifact.source_revision);
}

bool ReportEngine::same_build(const ReportArtifactKey &lhs,
                              const ReportArtifactKey &rhs) {
    return build_key(lhs) == build_key(rhs);
}

bool ReportEngine::start_next(uint32_t now_ms) {
    ReportArtifactRequest request;
    const ReportRequestSelection selected = queue_.take_next(now_ms, request);
    if (selected != ReportRequestSelection::Ready) return false;
    return start_request(request, now_ms);
}

bool ReportEngine::start_request(ReportArtifactRequest request,
                                 uint32_t now_ms) {
    active_request_ = request;
    active_ = true;

    ReportPlanRequest plan_request;
    plan_request.artifact = request.artifact;
    plan_request.signal_mask = report_signal_mask_all();
    plan_request.event_mask = REPORT_EVENT_ALL;

    ReportPlanResult planned = ReportPlanner::build(plan_request, catalog_);
    if (!planned.ready()) {
        if (planned.status == ReportPlanStatus::AllocationFailed &&
            retry_active(now_ms, PLAN_RETRY_DELAY_MS)) {
            return true;
        }

        complete_active(OperationOutcome::failed(),
                        planned.status,
                        ReportExecutorError::None);
        return true;
    }

    active_plan_ = std::move(planned.plan);
    if (!assembler_->begin_build(active_request_, *active_plan_)) {
        assembler_->discard_build();
        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::SinkRejected);
        return true;
    }

    const OperationAdmission admitted = executor_.start(
        active_plan_, *assembler_, active_request_.ticket.generation);
    if (admitted != OperationAdmission::Accepted) {
        const ReportExecutorError error = executor_.status().error;
        assembler_->discard_build();
        if (error == ReportExecutorError::AllocationFailed &&
            retry_active(now_ms, PLAN_RETRY_DELAY_MS)) {
            return true;
        }

        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::Ready,
                        error);
        return true;
    }

    if (executor_.status().terminal()) return finish_execution(now_ms);
    return true;
}

bool ReportEngine::finish_execution(uint32_t now_ms) {
    const ReportExecutorStatus executor_status = executor_.status();
    if (executor_status.state == ReportExecutorState::Complete) {
        const bool finished = assembler_->finish_build();
        std::shared_ptr<const ReportArtifactBundle> bundle =
            finished ? assembler_->take_completed() : nullptr;
        if (!finished || !bundle || !bundle->valid()) {
            assembler_->discard_build();
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::SinkRejected,
                            "report_artifact_assembly_failed");
            return true;
        }

        const StorageAtomicWriteLane lane =
            active_request_.priority == ReportRequestPriority::Foreground
                ? StorageAtomicWriteLane::Foreground
                : StorageAtomicWriteLane::Maintenance;
        const OperationAdmission admitted = artifact_store_.start(
            std::move(bundle), active_request_.ticket.generation, lane);
        if (admitted != OperationAdmission::Accepted) {
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_artifact_publish_rejected");
            return true;
        }

        executor_.reset();
        active_plan_.reset();
        publishing_ = true;
        return true;
    }

    assembler_->discard_build();
    if (executor_status.state == ReportExecutorState::Cancelled) {
        complete_active(OperationOutcome::cancelled(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        return true;
    }

    if (executor_status.error == ReportExecutorError::StorageRejected &&
        retry_active(now_ms, PLAN_RETRY_DELAY_MS)) {
        return true;
    }

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::Ready,
                    executor_status.error);
    return true;
}

bool ReportEngine::finish_publication() {
    const ReportArtifactStoreStatus store_status = artifact_store_.status();
    if (store_status.state == ReportArtifactStoreState::Ready) {
        published_ = artifact_store_.published();
        const bool valid = published_ && published_->valid();
        complete_active(valid ? OperationOutcome::succeeded()
                              : OperationOutcome::failed(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None,
                        valid ? nullptr : "report_artifact_publish_missing");
        return true;
    }

    if (store_status.state == ReportArtifactStoreState::Cancelled) {
        complete_active(OperationOutcome::cancelled(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        return true;
    }

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::Ready,
                    ReportExecutorError::None,
                    store_status.error[0]
                        ? store_status.error
                        : "report_artifact_publish_failed");
    return true;
}

bool ReportEngine::retry_active(uint32_t now_ms, uint32_t delay_ms) {
    const OperationOutcome outcome = queue_.retry(
        active_request_, now_ms, delay_ms, PLAN_RETRY_LIMIT);
    if (!outcome.should_retry()) return false;

    reset_active();
    return true;
}

void ReportEngine::cancel_active_work() {
    if (!active_) return;
    if (publishing_) {
        artifact_store_.cancel();
    } else {
        executor_.cancel();
    }
}

void ReportEngine::complete_active(OperationOutcome outcome,
                                   ReportPlanStatus plan_status,
                                   ReportExecutorError executor_error,
                                   const char *error) {
    last_completion_.request = active_request_;
    last_completion_.outcome = outcome;
    last_completion_.plan_status = plan_status;
    last_completion_.executor_error = executor_error;
    copy_cstr(last_completion_.error,
              sizeof(last_completion_.error),
              error ? error : "");
    reset_active();
}

void ReportEngine::reset_active() {
    executor_.reset();
    artifact_store_.reset();
    active_plan_.reset();
    active_request_ = {};
    active_ = false;
    publishing_ = false;
}

}  // namespace aircannect
