#include "report_engine.h"

#include <utility>

#include "report_sources.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t PLAN_RETRY_DELAY_MS = 50;
constexpr uint8_t PLAN_RETRY_LIMIT = 2;

StorageReadLane lookup_lane(ReportRequestPriority priority) {
    return priority == ReportRequestPriority::Foreground
        ? StorageReadLane::Foreground
        : StorageReadLane::Report;
}

StorageAtomicWriteLane write_lane(ReportRequestPriority priority) {
    return priority == ReportRequestPriority::Foreground
        ? StorageAtomicWriteLane::Foreground
        : StorageAtomicWriteLane::Maintenance;
}

bool catalog_contains_fallback(const NightCatalog &catalog,
                               const NightCatalogRecord &night,
                               uint64_t identity) {
    if (identity == 0) return false;

    size_t count = 0;
    const NightCatalogFallbackFile *files =
        catalog.fallback_files(night, count);
    for (size_t i = 0; files && i < count; ++i) {
        if (files[i].identity == identity) return true;
    }
    return false;
}

}  // namespace

ReportEngine::ReportEngine(ReportArtifactRequest *queue_slots,
                           size_t queue_capacity) :
    queue_(queue_slots, queue_capacity) {}

void ReportEngine::begin(StorageReadPort &read_port,
                         StorageAtomicWritePort &write_port,
                         ReportSpoolPort &spool_port,
                         ReportArtifactAssembler &assembler) {
    read_port_ = &read_port;
    assembler_ = &assembler;
    lookup_.begin(read_port);
    fallback_acquisition_.begin(read_port, write_port, spool_port);
    executor_.begin(read_port);
    artifact_store_.begin(read_port, write_port);
}

void ReportEngine::publish_catalog(std::shared_ptr<const NightCatalog> catalog) {
    catalog_ = std::move(catalog);

    if (published_) {
        const NightCatalogRecord *published_night =
            catalog_ ? catalog_->find(published_->key.sleep_day) : nullptr;
        if (!published_night ||
            published_night->source_revision !=
                published_->key.source_revision) {
            published_.reset();
        }
    }

    if (available_.request.valid()) {
        const NightCatalogRecord *available_night =
            catalog_ ? catalog_->find(available_.request.sleep_day) : nullptr;
        if (!available_night ||
            available_night->source_revision !=
                available_.request.source_revision) {
            available_ = {};
        }
    }

    if (phase_ == ActivePhase::Idle || !catalog_) return;

    const NightCatalogRecord *night =
        catalog_->find(active_request_.artifact.sleep_day);

    if (phase_ == ActivePhase::WaitingForCatalog) {
        if (!night ||
            !catalog_contains_fallback(*catalog_,
                                       *night,
                                       awaited_fallback_identity_)) {
            return;
        }

        ReportArtifactRequest resumed = active_request_;
        resumed.artifact.source_revision = night->source_revision;
        fallback_acquisition_.reset();
        active_plan_.reset();
        awaited_fallback_identity_ = 0;
        phase_ = ActivePhase::Idle;
        (void)start_request(resumed);
        return;
    }

    if (!night ||
        night->source_revision != active_request_.artifact.source_revision) {
        cancel_active_work();
    }
}

bool ReportEngine::catalog_update_required() const {
    return phase_ == ActivePhase::WaitingForCatalog;
}

std::shared_ptr<const LargeByteBuffer>
ReportEngine::fallback_replacement() const {
    return catalog_update_required()
        ? fallback_acquisition_.replacement()
        : nullptr;
}

void ReportEngine::catalog_update_failed(const char *error) {
    if (!catalog_update_required()) return;

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::InvalidCatalog,
                    ReportExecutorError::None,
                    error && error[0]
                        ? error
                        : "fallback_catalog_update_failed");
}

ReportRequestEnqueueResult ReportEngine::request(
    const ReportArtifactKey &artifact,
    ReportRequestPriority priority,
    uint32_t generation) {
    const ReportArtifactKey canonical = build_key(artifact);
    if (!canonical.valid() || generation == 0) return {};
    if (catalog_ && !artifact_current(canonical)) return {};

    if (phase_ != ActivePhase::Idle &&
        same_build(active_request_.artifact, canonical)) {
        if (active_request_.artifact == canonical &&
            report_request_priority_higher(
                priority, active_request_.priority)) {
            active_request_.priority = priority;
        }
        if (active_request_.artifact == canonical) {
            return {ReportRequestEnqueueStatus::AlreadyQueued,
                    active_request_.ticket};
        }
        cancel_active_work();
    }

    const ReportRequestEnqueueResult queued =
        queue_.enqueue(canonical, priority, generation);
    const bool accepted =
        queued.status != ReportRequestEnqueueStatus::Full &&
        queued.status != ReportRequestEnqueueStatus::Invalid;
    const bool can_preempt =
        accepted && phase_ != ActivePhase::Idle &&
        phase_ != ActivePhase::Publishing &&
        report_request_priority_higher(priority, active_request_.priority) &&
        artifact_current(active_request_.artifact);
    if (!can_preempt) return queued;

    const ReportRequestEnqueueResult restored = queue_.enqueue(
        active_request_.artifact,
        active_request_.priority,
        active_request_.ticket.generation);
    if (restored.status != ReportRequestEnqueueStatus::Full &&
        restored.status != ReportRequestEnqueueStatus::Invalid) {
        cancel_active_work();
    }
    return queued;
}

size_t ReportEngine::cancel_generation(uint32_t generation) {
    size_t cancelled = queue_.cancel_generation(generation);
    if (phase_ != ActivePhase::Idle &&
        active_request_.priority == ReportRequestPriority::Foreground &&
        active_request_.ticket.generation == generation) {
        cancel_active_work();
        ++cancelled;
    }
    return cancelled;
}

void ReportEngine::clear() {
    queue_.clear();

    if (phase_ == ActivePhase::AcquiringFallback) {
        fallback_acquisition_.cancel();
        clear_after_fallback_cancel_ = true;
        available_ = {};
        published_.reset();
        last_completion_ = {};
        return;
    }

    if (phase_ != ActivePhase::Idle) {
        const bool discard_assembly = phase_ == ActivePhase::Executing;
        cancel_active_work();
        if (assembler_ && discard_assembly) {
            assembler_->discard_build();
        }
    }

    reset_active();
    available_ = {};
    published_.reset();
    last_completion_ = {};
}

bool ReportEngine::poll(uint32_t now_ms, size_t record_budget) {
    if (!read_port_ || !assembler_) return false;

    bool worked = false;
    if (phase_ == ActivePhase::Idle) {
        if (queue_.size() == 0) return false;
        if (!catalog_) return false;
        worked = start_next(now_ms);
    }
    if (phase_ == ActivePhase::Idle) return worked;

    switch (phase_) {
        case ActivePhase::LookingUp:
            worked = lookup_.poll() || worked;
            if (lookup_.status().terminal()) {
                worked = finish_lookup(now_ms) || worked;
            }
            break;

        case ActivePhase::AcquiringFallback:
            worked = fallback_acquisition_.poll() || worked;
            if (fallback_acquisition_.status().terminal()) {
                worked = finish_fallback_acquisition() || worked;
            }
            break;

        case ActivePhase::WaitingForCatalog:
            break;

        case ActivePhase::Executing:
            worked = executor_.poll(record_budget) || worked;
            if (executor_.status().terminal()) {
                worked = finish_execution(now_ms) || worked;
            }
            break;

        case ActivePhase::Publishing:
            worked = artifact_store_.poll() || worked;
            if (artifact_store_.status().terminal()) {
                worked = finish_publication(now_ms) || worked;
            }
            break;

        case ActivePhase::Idle:
            break;
    }
    return worked;
}

ReportEngineStatus ReportEngine::status() const {
    ReportEngineStatus out;
    out.queued = queue_.size();
    out.active_request = active_request_;
    out.lookup = lookup_.status();
    out.fallback = fallback_acquisition_.status();
    out.executor = executor_.status();
    out.store = artifact_store_.status();
    out.awaited_fallback_identity = awaited_fallback_identity_;
    out.last_completion = last_completion_;

    switch (phase_) {
        case ActivePhase::LookingUp:
            out.state = ReportEngineState::LookingUp;
            break;
        case ActivePhase::AcquiringFallback:
            out.state = ReportEngineState::AcquiringFallback;
            break;
        case ActivePhase::WaitingForCatalog:
            out.state = ReportEngineState::WaitingForCatalog;
            break;
        case ActivePhase::Executing:
            out.state = ReportEngineState::Executing;
            break;
        case ActivePhase::Publishing:
            out.state = ReportEngineState::Publishing;
            break;
        case ActivePhase::Idle:
            if (queue_.size() > 0 && !catalog_) {
                out.state = ReportEngineState::WaitingForCatalog;
            } else if (queue_.size() > 0) {
                out.state = ReportEngineState::Queued;
            }
            break;
    }
    return out;
}

std::shared_ptr<const ReportArtifactBundle> ReportEngine::take_published() {
    return std::move(published_);
}

ReportArtifactAvailability ReportEngine::take_available() {
    ReportArtifactAvailability out = available_;
    available_ = {};
    return out;
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

bool ReportEngine::artifact_current(
    const ReportArtifactKey &artifact) const {
    if (!catalog_ || !artifact.valid()) return false;

    const NightCatalogRecord *night = catalog_->find(artifact.sleep_day);
    return night && night->source_revision == artifact.source_revision;
}

bool ReportEngine::start_next(uint32_t now_ms) {
    ReportArtifactRequest request;
    const ReportRequestSelection selected = queue_.take_next(now_ms, request);
    if (selected != ReportRequestSelection::Ready) return false;
    if (!artifact_current(request.artifact)) {
        active_request_ = request;
        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::StaleRevision,
                        ReportExecutorError::None,
                        "report_artifact_revision_stale");
        return true;
    }
    return start_request(request);
}

bool ReportEngine::start_request(ReportArtifactRequest request) {
    active_request_ = request;
    active_availability_ = {};
    active_availability_.request = request.artifact;

    const OperationAdmission admitted = lookup_.start(
        request.artifact,
        request.ticket.generation,
        lookup_lane(request.priority));
    if (admitted != OperationAdmission::Accepted) {
        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None,
                        "report_artifact_lookup_rejected");
        return true;
    }

    phase_ = ActivePhase::LookingUp;
    return true;
}

bool ReportEngine::finish_lookup(uint32_t now_ms) {
    const ReportArtifactLookupStatus lookup_status = lookup_.status();
    switch (lookup_status.state) {
        case ReportArtifactLookupState::Ready:
            active_availability_ = lookup_.availability();
            if (!active_availability_.requested_ready()) {
                complete_active(OperationOutcome::failed(),
                                ReportPlanStatus::Ready,
                                ReportExecutorError::None,
                                "report_artifact_lookup_missing");
                return true;
            }

            available_ = active_availability_;
            complete_active(OperationOutcome::succeeded(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None);
            return true;

        case ReportArtifactLookupState::MissingManifest: {
            lookup_.reset();
            active_availability_ = {};
            active_availability_.request = active_request_.artifact;
            build_tile_after_pair_ =
                active_request_.artifact.kind ==
                ReportArtifactKind::RangeTile;
            const ReportArtifactKey artifact = build_tile_after_pair_
                ? ReportArtifactKey::result(
                      active_request_.artifact.sleep_day,
                      active_request_.artifact.source_revision)
                : active_request_.artifact;
            return start_build(artifact, now_ms);
        }

        case ReportArtifactLookupState::MissingArtifact:
            active_availability_ = lookup_.availability();
            lookup_.reset();
            if (!active_availability_.pair_ready() ||
                active_request_.artifact.kind !=
                    ReportArtifactKind::RangeTile) {
                complete_active(OperationOutcome::failed(),
                                ReportPlanStatus::Ready,
                                ReportExecutorError::None,
                                "report_artifact_manifest_invalid");
                return true;
            }
            return start_build(active_request_.artifact, now_ms);

        case ReportArtifactLookupState::Cancelled:
            complete_active(OperationOutcome::cancelled(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None);
            return true;

        case ReportArtifactLookupState::Failed:
            if (retry_active(now_ms, PLAN_RETRY_DELAY_MS)) return true;
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            lookup_status.error[0]
                                ? lookup_status.error
                                : "report_artifact_lookup_failed");
            return true;

        case ReportArtifactLookupState::Idle:
        case ReportArtifactLookupState::SubmitManifest:
        case ReportArtifactLookupState::WaitManifest:
            return false;
    }
    return false;
}

bool ReportEngine::start_build(const ReportArtifactKey &artifact,
                               uint32_t now_ms) {
    if (!artifact.valid() ||
        artifact.sleep_day != active_request_.artifact.sleep_day ||
        artifact.source_revision !=
            active_request_.artifact.source_revision) {
        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::InvalidRequest,
                        ReportExecutorError::None,
                        "report_artifact_build_key_invalid");
        return true;
    }

    build_request_ = active_request_;
    build_request_.artifact = artifact;

    ReportPlanRequest plan_request;
    plan_request.artifact = artifact;
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
    if (active_plan_->acquirable_signal_mask() != 0 ||
        active_plan_->missing_event_mask() != 0) {
        const OperationAdmission admitted = fallback_acquisition_.start(
            active_plan_,
            active_request_.ticket.generation,
            lookup_lane(active_request_.priority),
            write_lane(active_request_.priority));
        if (admitted != OperationAdmission::Accepted) {
            const ReportFallbackAcquisitionStatus fallback_status =
                fallback_acquisition_.status();
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            fallback_status.error[0]
                                ? fallback_status.error
                                : "fallback_acquisition_rejected");
            return true;
        }

        phase_ = ActivePhase::AcquiringFallback;
        return true;
    }

    if (!assembler_->begin_build(build_request_, *active_plan_)) {
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

    phase_ = ActivePhase::Executing;
    if (executor_.status().terminal()) return finish_execution(now_ms);
    return true;
}

bool ReportEngine::finish_fallback_acquisition() {
    const ReportFallbackAcquisitionStatus fallback_status =
        fallback_acquisition_.status();
    if (clear_after_fallback_cancel_) {
        clear_after_fallback_cancel_ = false;
        reset_active();
        last_completion_ = {};
        return true;
    }

    if (fallback_status.state == ReportFallbackAcquisitionState::Ready &&
        fallback_status.replacement_identity != 0) {
        awaited_fallback_identity_ = fallback_status.replacement_identity;
        active_plan_.reset();
        phase_ = ActivePhase::WaitingForCatalog;
        return true;
    }

    if (fallback_status.state ==
        ReportFallbackAcquisitionState::Cancelled) {
        complete_active(OperationOutcome::cancelled(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        return true;
    }

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::Ready,
                    ReportExecutorError::None,
                    fallback_status.error[0]
                        ? fallback_status.error
                        : "fallback_acquisition_failed");
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

        const OperationAdmission admitted = artifact_store_.start(
            std::move(bundle),
            active_request_.ticket.generation,
            write_lane(active_request_.priority));
        if (admitted != OperationAdmission::Accepted) {
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_artifact_publish_rejected");
            return true;
        }

        executor_.reset();
        active_plan_.reset();
        phase_ = ActivePhase::Publishing;
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

bool ReportEngine::finish_publication(uint32_t now_ms) {
    const ReportArtifactStoreStatus store_status = artifact_store_.status();
    if (store_status.state == ReportArtifactStoreState::Ready) {
        std::shared_ptr<const ReportArtifactBundle> bundle =
            artifact_store_.published();
        if (!bundle || !bundle->valid() ||
            !active_availability_.merge(*bundle)) {
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_artifact_publish_missing");
            return true;
        }

        if (build_tile_after_pair_ &&
            bundle->key.kind == ReportArtifactKind::Result) {
            artifact_store_.reset();
            build_tile_after_pair_ = false;
            return start_build(active_request_.artifact, now_ms);
        }

        if (!active_availability_.requested_ready()) {
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_artifact_publish_incomplete");
            return true;
        }

        published_ = std::move(bundle);
        available_ = active_availability_;
        complete_active(OperationOutcome::succeeded(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
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
    switch (phase_) {
        case ActivePhase::LookingUp:
            lookup_.cancel();
            break;
        case ActivePhase::AcquiringFallback:
            fallback_acquisition_.cancel();
            break;
        case ActivePhase::WaitingForCatalog:
            complete_active(OperationOutcome::cancelled(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None);
            break;
        case ActivePhase::Executing:
            executor_.cancel();
            break;
        case ActivePhase::Publishing:
            artifact_store_.cancel();
            break;
        case ActivePhase::Idle:
            break;
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
    lookup_.reset();
    executor_.reset();
    artifact_store_.reset();
    fallback_acquisition_.reset();
    active_plan_.reset();
    active_request_ = {};
    build_request_ = {};
    active_availability_ = {};
    phase_ = ActivePhase::Idle;
    build_tile_after_pair_ = false;
    awaited_fallback_identity_ = 0;
    clear_after_fallback_cancel_ = false;
}

}  // namespace aircannect
