#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_executor.h"
#include "report_planner.h"
#include "report_request_queue.h"

namespace aircannect {

enum class ReportEngineState : uint8_t {
    Idle,
    Queued,
    WaitingForCatalog,
    Executing,
};

struct ReportEngineCompletion {
    ReportArtifactRequest request;
    OperationOutcome outcome = OperationOutcome::failed();
    ReportPlanStatus plan_status = ReportPlanStatus::InvalidRequest;
    ReportExecutorError executor_error = ReportExecutorError::None;

    bool valid() const { return request.ticket.valid(); }
};

struct ReportEngineStatus {
    ReportEngineState state = ReportEngineState::Idle;
    size_t queued = 0;
    ReportArtifactRequest active_request;
    ReportExecutorStatus executor;
    ReportEngineCompletion last_completion;
};

class ReportArtifactAssembler : public ReportExecutionSink {
public:
    ~ReportArtifactAssembler() override = default;

    virtual bool begin_build(const ReportArtifactRequest &request,
                             const ReportReadPlan &plan) = 0;
    virtual bool finish_build() = 0;
    virtual void discard_build() = 0;
};

class ReportEngine {
public:
    ReportEngine(ReportArtifactRequest *queue_slots, size_t queue_capacity);

    ReportEngine(const ReportEngine &) = delete;
    ReportEngine &operator=(const ReportEngine &) = delete;

    void begin(StorageReadPort &read_port, ReportArtifactAssembler &assembler);

    void publish_catalog(std::shared_ptr<const NightCatalog> catalog);
    ReportRequestEnqueueResult request(
        const ReportArtifactKey &artifact,
        ReportRequestPriority priority,
        uint32_t generation);
    size_t cancel_generation(uint32_t generation);
    void clear();

    bool poll(uint32_t now_ms, size_t record_budget = 1);
    ReportEngineStatus status() const;

private:
    static ReportArtifactKey build_key(const ReportArtifactKey &artifact);
    static bool same_build(const ReportArtifactKey &lhs,
                           const ReportArtifactKey &rhs);

    bool start_next(uint32_t now_ms);
    bool start_request(ReportArtifactRequest request, uint32_t now_ms);
    bool finish_execution(uint32_t now_ms);
    bool retry_active(uint32_t now_ms, uint32_t delay_ms);
    void complete_active(OperationOutcome outcome,
                         ReportPlanStatus plan_status,
                         ReportExecutorError executor_error);
    void reset_active();

    ReportRequestQueue queue_;
    ReportExecutor executor_;
    StorageReadPort *read_port_ = nullptr;
    ReportArtifactAssembler *assembler_ = nullptr;
    std::shared_ptr<const NightCatalog> catalog_;
    std::shared_ptr<const ReportReadPlan> active_plan_;
    ReportArtifactRequest active_request_;
    ReportEngineCompletion last_completion_;
    bool active_ = false;
};

}  // namespace aircannect
