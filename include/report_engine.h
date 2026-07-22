#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_artifact_lookup_service.h"
#include "report_artifact_store_service.h"
#include "report_executor.h"
#include "report_fallback_acquisition_service.h"
#include "report_planner.h"
#include "report_request_queue.h"

namespace aircannect {

enum class ReportEngineState : uint8_t {
    Idle,
    Queued,
    WaitingForCatalog,
    LookingUp,
    AcquiringFallback,
    Executing,
    Publishing,
};

struct ReportEngineCompletion {
    ReportArtifactRequest request;
    OperationOutcome outcome = OperationOutcome::failed();
    ReportPlanStatus plan_status = ReportPlanStatus::InvalidRequest;
    ReportExecutorError executor_error = ReportExecutorError::None;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool valid() const { return request.ticket.valid(); }
};

struct ReportEngineStatus {
    ReportEngineState state = ReportEngineState::Idle;
    size_t queued = 0;
    bool foreground_active = false;
    ReportArtifactRequest active_request;
    ReportArtifactLookupStatus lookup;
    ReportFallbackAcquisitionStatus fallback;
    ReportExecutorStatus executor;
    ReportArtifactStoreStatus store;
    uint64_t awaited_fallback_identity = 0;
    ReportEngineCompletion last_completion;
};

class ReportArtifactAssembler : public ReportExecutionSink {
public:
    ~ReportArtifactAssembler() override = default;

    virtual bool begin_build(const ReportArtifactRequest &request,
                             const ReportReadPlan &plan) = 0;
    virtual bool finish_build() = 0;
    virtual void discard_build() = 0;
    virtual std::shared_ptr<const ReportArtifactBundle> take_completed() = 0;
    virtual const char *failure_reason() const { return nullptr; }
};

class ReportEngine {
public:
    ReportEngine(ReportArtifactRequest *queue_slots, size_t queue_capacity);

    ReportEngine(const ReportEngine &) = delete;
    ReportEngine &operator=(const ReportEngine &) = delete;

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port,
               ReportSpoolPort &spool_port,
               ReportArtifactAssembler &assembler);

    void publish_catalog(std::shared_ptr<const NightCatalog> catalog);
    bool catalog_update_required() const;
    std::shared_ptr<const LargeByteBuffer> fallback_replacement() const;
    void catalog_update_failed(const char *error);

    ReportRequestEnqueueResult request(
        const ReportArtifactKey &artifact,
        ReportRequestPriority priority,
        uint32_t generation);
    size_t cancel_background();
    void clear();

    bool poll(uint32_t now_ms, size_t record_budget = 1);
    ReportEngineStatus status() const;
    ReportArtifactAvailability take_available();

private:
    enum class ActivePhase : uint8_t {
        Idle,
        LookingUp,
        AcquiringFallback,
        WaitingForCatalog,
        Executing,
        Publishing,
    };

    static ReportArtifactKey build_key(const ReportArtifactKey &artifact);
    static bool same_build(const ReportArtifactKey &lhs,
                           const ReportArtifactKey &rhs);

    bool artifact_current(const ReportArtifactKey &artifact) const;
    bool start_next(uint32_t now_ms);
    bool start_request(ReportArtifactRequest request);
    bool finish_lookup(uint32_t now_ms);
    bool start_build(const ReportArtifactKey &artifact, uint32_t now_ms);
    bool finish_fallback_acquisition();
    bool finish_execution(uint32_t now_ms);
    bool finish_publication(uint32_t now_ms);
    bool retry_active(uint32_t now_ms, uint32_t delay_ms);
    void cancel_active_work();
    void complete_active(OperationOutcome outcome,
                         ReportPlanStatus plan_status,
                         ReportExecutorError executor_error,
                         const char *error = nullptr);
    void reset_active();

    ReportRequestQueue queue_;
    ReportArtifactLookupService lookup_;
    ReportFallbackAcquisitionService fallback_acquisition_;
    ReportExecutor executor_;
    ReportArtifactStoreService artifact_store_;
    StorageReadPort *read_port_ = nullptr;
    ReportArtifactAssembler *assembler_ = nullptr;
    std::shared_ptr<const NightCatalog> catalog_;
    std::shared_ptr<const ReportReadPlan> active_plan_;
    ReportArtifactRequest active_request_;
    ReportArtifactRequest build_request_;
    ReportEngineCompletion last_completion_;
    ReportArtifactAvailability active_availability_;
    ReportArtifactAvailability available_;
    uint64_t awaited_fallback_identity_ = 0;
    ActivePhase phase_ = ActivePhase::Idle;
    bool build_tile_after_pair_ = false;
    bool clear_after_fallback_cancel_ = false;
};

}  // namespace aircannect
