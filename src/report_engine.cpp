#include "report_engine.h"

#include <utility>

#include "report_sources.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t PLAN_RETRY_DELAY_MS = 50;
constexpr uint8_t PLAN_RETRY_LIMIT = 2;

StorageReadLane read_lane(ReportRequestPriority priority) {
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

const char *plan_status_error(ReportPlanStatus status) {
    switch (status) {
        case ReportPlanStatus::InvalidRequest:
            return "report_plan_invalid_request";
        case ReportPlanStatus::NightMissing:
            return "report_plan_night_missing";
        case ReportPlanStatus::StaleRevision:
            return "report_plan_revision_stale";
        case ReportPlanStatus::InvalidCatalog:
            return "report_plan_catalog_invalid";
        case ReportPlanStatus::AllocationFailed:
            return "report_plan_allocation_failed";
        case ReportPlanStatus::Ready:
        default:
            return nullptr;
    }
}

const char *executor_error_name(ReportExecutorError error) {
    switch (error) {
        case ReportExecutorError::InvalidArgument:
            return "report_executor_invalid_argument";
        case ReportExecutorError::InvalidPlan:
            return "report_executor_invalid_plan";
        case ReportExecutorError::AllocationFailed:
            return "report_executor_allocation_failed";
        case ReportExecutorError::StorageRejected:
            return "report_executor_storage_rejected";
        case ReportExecutorError::StorageFailed:
            return "report_executor_storage_failed";
        case ReportExecutorError::StorageShortRead:
            return "report_executor_storage_short_read";
        case ReportExecutorError::DecodeFailed:
            return "report_executor_decode_failed";
        case ReportExecutorError::SinkRejected:
            return "report_executor_sink_rejected";
        case ReportExecutorError::None:
        default:
            return nullptr;
    }
}

const char *completion_error(OperationOutcome outcome,
                             ReportPlanStatus plan_status,
                             ReportExecutorError executor_error,
                             const char *explicit_error) {
    if (explicit_error && explicit_error[0]) return explicit_error;
    if (outcome.disposition != OperationDisposition::Failed) return "";

    const char *executor = executor_error_name(executor_error);
    if (executor) return executor;

    const char *plan = plan_status_error(plan_status);
    return plan ? plan : "report_engine_failed";
}

uint32_t increment_generation(uint32_t generation) {
    ++generation;
    return generation == 0 ? 1 : generation;
}

}  // namespace

ReportEngine::ReportEngine(ReportArtifactRequest *queue_slots,
                           size_t queue_capacity) :
    queue_(queue_slots, queue_capacity) {}

void ReportEngine::begin(StorageReadPort &read_port,
                         StorageAtomicWritePort &write_port,
                         ReportSpoolPort &spool_port) {
    fallback_acquisition_.begin(read_port, write_port, spool_port);
    executor_.begin(read_port);
    store_.begin(write_port);
}

void ReportEngine::publish_catalog(
    std::shared_ptr<const NightCatalog> catalog) {
    catalog_ = std::move(catalog);
    if (phase_ == ActivePhase::Idle || !catalog_) return;

    const NightCatalogRecord *night =
        catalog_->find(active_request_.artifact.sleep_day);
    if (phase_ == ActivePhase::WaitingForCatalog) {
        if (!night ||
            !catalog_contains_fallback(
                *catalog_, *night, awaited_fallback_identity_)) {
            return;
        }

        ReportArtifactRequest resumed = active_request_;
        resumed.artifact = ReportArtifactKey::result(
            night->sleep_day, night->source_revision);
        resumed.force_rebuild = false;
        fallback_acquisition_.reset();
        active_plan_.reset();
        awaited_fallback_identity_ = 0;
        phase_ = ActivePhase::Idle;
        (void)start_request(resumed, 0);
        return;
    }

    if (!night ||
        night->source_revision != active_request_.artifact.source_revision) {
        cancel_active_work();
    }
}

void ReportEngine::publish_store_catalog(
    std::shared_ptr<const ReportSignalStoreCatalog> catalog) {
    store_catalog_ = std::move(catalog);
}

void ReportEngine::publish_spool_availability(
    const ReportSpoolAvailability &availability,
    bool complete) {
    spool_availability_ = availability;
    spool_availability_complete_ = complete;
    fallback_acquisition_.publish_spool_availability(availability);
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
    uint32_t generation,
    bool force_rebuild) {
    if (!artifact.valid() || generation == 0) {
        return {};
    }
    if (catalog_ && !source_current(artifact)) return {};

    if (phase_ != ActivePhase::Idle &&
        active_request_.artifact.sleep_day == artifact.sleep_day) {
        const bool same_source = active_request_.artifact == artifact;
        const bool rebuild_upgrade = same_source && force_rebuild &&
            !active_request_.force_rebuild;
        if (!rebuild_upgrade && same_source) {
            if (report_request_priority_higher(
                    priority, active_request_.priority)) {
                active_request_.priority = priority;
            }
            return {ReportRequestEnqueueStatus::AlreadyQueued,
                    active_request_.ticket};
        }
        cancel_active_work();
    }

    const ReportRequestEnqueueResult queued = queue_.enqueue(
        artifact, priority, generation, force_rebuild);
    const bool accepted =
        queued.status != ReportRequestEnqueueStatus::Full &&
        queued.status != ReportRequestEnqueueStatus::Invalid;
    const bool can_preempt = accepted && phase_ != ActivePhase::Idle &&
        report_request_priority_higher(priority, active_request_.priority) &&
        source_current(active_request_.artifact);
    if (!can_preempt) return queued;

    const ReportRequestEnqueueResult restored = queue_.enqueue(
        active_request_.artifact,
        active_request_.priority,
        active_request_.ticket.generation,
        active_request_.force_rebuild);
    if (restored.status != ReportRequestEnqueueStatus::Full &&
        restored.status != ReportRequestEnqueueStatus::Invalid) {
        cancel_active_work();
    }
    return queued;
}

size_t ReportEngine::cancel_background() {
    size_t cancelled = queue_.cancel_background();
    if (phase_ != ActivePhase::Idle &&
        active_request_.priority != ReportRequestPriority::Foreground) {
        cancel_active_work();
        ++cancelled;
    }
    return cancelled;
}

void ReportEngine::clear() {
    queue_.clear();
    published_ = {};

    if (phase_ == ActivePhase::AcquiringFallback) {
        fallback_acquisition_.cancel();
        clear_after_fallback_cancel_ = true;
        last_completion_ = {};
        return;
    }

    if (phase_ == ActivePhase::Executing) builder_.discard_build();
    if (phase_ != ActivePhase::Idle) cancel_active_work();

    reset_active();
    last_completion_ = {};
}

bool ReportEngine::poll(uint32_t now_ms, size_t record_budget) {
    bool worked = false;
    if (phase_ == ActivePhase::Idle) {
        if (queue_.size() == 0 || !catalog_) return false;
        worked = start_next(now_ms);
    }
    if (phase_ == ActivePhase::Idle) return worked;

    switch (phase_) {
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
            worked = store_.poll() || worked;
            if (store_.status().terminal()) {
                worked = finish_publication() || worked;
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
    out.foreground_active =
        queue_.contains(ReportRequestPriority::Foreground) ||
        (phase_ != ActivePhase::Idle &&
         active_request_.priority == ReportRequestPriority::Foreground);
    out.fallback = fallback_acquisition_.status();
    out.executor = executor_.status();
    out.store = store_.status();
    out.awaited_fallback_identity = awaited_fallback_identity_;
    out.last_completion = last_completion_;

    switch (phase_) {
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

ReportSignalStoreCatalogInput ReportEngine::take_published() {
    ReportSignalStoreCatalogInput out = std::move(published_);
    published_ = {};
    return out;
}

bool ReportEngine::source_current(const ReportArtifactKey &artifact) const {
    if (!catalog_ || !artifact.valid()) {
        return false;
    }

    const NightCatalogRecord *night = catalog_->find(artifact.sleep_day);
    return night && night->source_revision == artifact.source_revision;
}

uint32_t ReportEngine::next_store_generation(SleepDayId sleep_day) const {
    const ReportSignalStoreCatalogRecord *record =
        store_catalog_ ? store_catalog_->find(sleep_day) : nullptr;
    return record ? increment_generation(record->generation) : 1;
}

bool ReportEngine::start_next(uint32_t now_ms) {
    ReportArtifactRequest request;
    const ReportRequestSelection selected = queue_.take_next(now_ms, request);
    if (selected != ReportRequestSelection::Ready) return false;
    if (!source_current(request.artifact)) {
        active_request_ = request;
        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::StaleRevision,
                        ReportExecutorError::None,
                        "report_source_revision_stale");
        return true;
    }
    return start_request(request, now_ms);
}

bool ReportEngine::start_request(ReportArtifactRequest request,
                                 uint32_t now_ms) {
    active_request_ = request;

    const ReportSignalStoreCatalogRecord *stored =
        store_catalog_ ? store_catalog_->find(request.artifact.sleep_day)
                       : nullptr;
    if (!request.force_rebuild && stored &&
        stored->source_revision == request.artifact.source_revision) {
        complete_active(OperationOutcome::succeeded(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        last_completion_.store_generation = stored->generation;
        return true;
    }

    active_store_generation_ = next_store_generation(
        request.artifact.sleep_day);
    return start_build(now_ms);
}

bool ReportEngine::start_build(uint32_t now_ms) {
    ReportPlanRequest plan_request;
    plan_request.artifact = active_request_.artifact;
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
    if (active_plan_->fallback_acquisition_allowed() &&
        (active_plan_->acquirable_signal_mask() != 0 ||
         active_plan_->missing_event_mask() != 0)) {
        if (active_request_.priority != ReportRequestPriority::Foreground &&
            !spool_availability_complete_) {
            complete_active(OperationOutcome::cancelled(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_spool_availability_pending");
            return true;
        }

        const OperationAdmission admitted = fallback_acquisition_.start(
            active_plan_,
            active_request_.ticket.generation,
            read_lane(active_request_.priority),
            write_lane(active_request_.priority),
            spool_availability_);
        if (admitted != OperationAdmission::Accepted) {
            const ReportFallbackAcquisitionStatus status =
                fallback_acquisition_.status();
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            status.error[0]
                                ? status.error
                                : "fallback_acquisition_rejected");
            return true;
        }

        phase_ = ActivePhase::AcquiringFallback;
        return true;
    }

    if (!builder_.begin_build(
            active_request_, *active_plan_, active_store_generation_)) {
        const char *reason = builder_.failure_reason();
        builder_.discard_build();
        complete_active(OperationOutcome::failed(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::SinkRejected,
                        reason ? reason : "report_signal_store_begin_failed");
        return true;
    }

    const OperationAdmission admitted = executor_.start(
        active_plan_, builder_, active_request_.ticket.generation);
    if (admitted != OperationAdmission::Accepted) {
        const ReportExecutorError error = executor_.status().error;
        builder_.discard_build();
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
    const ReportFallbackAcquisitionStatus status =
        fallback_acquisition_.status();
    if (clear_after_fallback_cancel_) {
        clear_after_fallback_cancel_ = false;
        reset_active();
        last_completion_ = {};
        return true;
    }

    if (status.state == ReportFallbackAcquisitionState::Ready &&
        status.replacement_identity != 0) {
        awaited_fallback_identity_ = status.replacement_identity;
        active_plan_.reset();
        phase_ = ActivePhase::WaitingForCatalog;
        return true;
    }

    if (status.state == ReportFallbackAcquisitionState::Cancelled) {
        complete_active(OperationOutcome::cancelled(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        return true;
    }

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::Ready,
                    ReportExecutorError::None,
                    status.error[0]
                        ? status.error
                        : "fallback_acquisition_failed",
                    status.source);
    return true;
}

bool ReportEngine::finish_execution(uint32_t now_ms) {
    const ReportExecutorStatus status = executor_.status();
    if (status.state == ReportExecutorState::Complete) {
        const bool finished = builder_.finish_build();
        std::shared_ptr<ReportSignalStoreBundle> bundle =
            finished ? builder_.take_completed() : nullptr;
        if (!finished || !bundle || !bundle->valid()) {
            const char *reason = builder_.failure_reason();
            builder_.discard_build();
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::SinkRejected,
                            reason ? reason
                                   : "report_signal_store_assembly_failed");
            return true;
        }

        const OperationAdmission admitted = store_.start(
            std::move(bundle),
            active_request_.ticket.generation,
            write_lane(active_request_.priority));
        if (admitted != OperationAdmission::Accepted) {
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_signal_store_publish_rejected");
            return true;
        }

        executor_.reset();
        active_plan_.reset();
        phase_ = ActivePhase::Publishing;
        return true;
    }

    const char *sink_reason =
        status.error == ReportExecutorError::SinkRejected
            ? builder_.failure_reason()
            : nullptr;
    builder_.discard_build();

    if (status.state == ReportExecutorState::Cancelled) {
        complete_active(OperationOutcome::cancelled(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        return true;
    }

    if (status.error == ReportExecutorError::StorageRejected &&
        retry_active(now_ms, PLAN_RETRY_DELAY_MS)) {
        return true;
    }

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::Ready,
                    status.error,
                    sink_reason);
    return true;
}

bool ReportEngine::finish_publication() {
    const ReportSignalStoreStatus status = store_.status();
    if (status.state == ReportSignalStoreState::Ready) {
        std::shared_ptr<const LargeByteBuffer> metadata =
            store_.take_published_metadata();
        ReportSignalStoreNightView view;
        if (!metadata ||
            !ReportSignalStoreNightCodec::decode(
                metadata->data(), metadata->size(), view) ||
            view.night.sleep_day != active_request_.artifact.sleep_day ||
            view.night.source_revision !=
                active_request_.artifact.source_revision) {
            complete_active(OperationOutcome::failed(),
                            ReportPlanStatus::Ready,
                            ReportExecutorError::None,
                            "report_signal_store_publish_missing");
            return true;
        }

        published_.metadata = std::move(metadata);
        complete_active(OperationOutcome::succeeded(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        last_completion_.store_generation = view.night.generation;
        return true;
    }

    if (status.state == ReportSignalStoreState::Cancelled) {
        complete_active(OperationOutcome::cancelled(),
                        ReportPlanStatus::Ready,
                        ReportExecutorError::None);
        return true;
    }

    complete_active(OperationOutcome::failed(),
                    ReportPlanStatus::Ready,
                    ReportExecutorError::None,
                    status.error[0]
                        ? status.error
                        : "report_signal_store_publish_failed");
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
            builder_.discard_build();
            break;
        case ActivePhase::Publishing:
            store_.cancel();
            break;
        case ActivePhase::Idle:
            break;
    }
}

void ReportEngine::complete_active(OperationOutcome outcome,
                                   ReportPlanStatus plan_status,
                                   ReportExecutorError executor_error,
                                   const char *error,
                                   ReportSourceId fallback_source) {
    last_completion_.request = active_request_;
    last_completion_.outcome = outcome;
    last_completion_.plan_status = plan_status;
    last_completion_.executor_error = executor_error;
    last_completion_.fallback_source = fallback_source;
    copy_cstr(last_completion_.error,
              sizeof(last_completion_.error),
              completion_error(outcome,
                               plan_status,
                               executor_error,
                               error));
    reset_active();
}

void ReportEngine::reset_active() {
    executor_.reset();
    builder_.discard_build();
    store_.reset();
    fallback_acquisition_.reset();
    active_plan_.reset();
    active_request_ = {};
    awaited_fallback_identity_ = 0;
    active_store_generation_ = 0;
    phase_ = ActivePhase::Idle;
    clear_after_fallback_cancel_ = false;
}

}  // namespace aircannect
