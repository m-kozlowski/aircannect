#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_executor.h"
#include "report_fallback_acquisition_service.h"
#include "report_planner.h"
#include "report_request_queue.h"
#include "report_signal_store_builder.h"
#include "report_signal_store_catalog.h"
#include "report_signal_store_service.h"
#include "report_spool_availability.h"
#include "storage_bounded_file_loader.h"

namespace aircannect {

enum class ReportEngineState : uint8_t {
    Idle,
    Queued,
    WaitingForCatalog,
    AcquiringFallback,
    Executing,
    Publishing,
};

struct ReportEngineCompletion {
    ReportArtifactRequest request;
    OperationOutcome outcome = OperationOutcome::failed();
    ReportPlanStatus plan_status = ReportPlanStatus::InvalidRequest;
    ReportExecutorError executor_error = ReportExecutorError::None;
    ReportSourceId fallback_source = ReportSourceId::Summary;
    uint32_t store_generation = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool valid() const { return request.ticket.valid(); }
};

struct ReportEngineStatus {
    ReportEngineState state = ReportEngineState::Idle;
    size_t queued = 0;
    bool foreground_active = false;
    ReportArtifactRequest active_request;
    ReportFallbackAcquisitionStatus fallback;
    ReportExecutorStatus executor;
    ReportSignalStoreStatus store;
    uint64_t awaited_fallback_identity = 0;
    ReportEngineCompletion last_completion;
};

class ReportEngine {
public:
    ReportEngine(ReportArtifactRequest *queue_slots, size_t queue_capacity);

    ReportEngine(const ReportEngine &) = delete;
    ReportEngine &operator=(const ReportEngine &) = delete;

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port,
               ReportSpoolPort &spool_port,
               StorageRangeWritePort &range_write_port);

    void publish_catalog(std::shared_ptr<const NightCatalog> catalog);
    void publish_store_catalog(
        std::shared_ptr<const ReportSignalStoreCatalog> catalog);
    void publish_spool_availability(
        const ReportSpoolAvailability &availability,
        bool complete);
    bool catalog_update_required() const;
    std::shared_ptr<const LargeByteBuffer> fallback_replacement() const;
    void catalog_update_failed(const char *error);

    ReportRequestEnqueueResult request(
        const ReportArtifactKey &artifact,
        ReportRequestPriority priority,
        uint32_t generation,
        bool force_rebuild = false);
    size_t cancel_background();
    void clear();

    bool poll(uint32_t now_ms, size_t record_budget = 1);
    ReportEngineStatus status() const;
    ReportSignalStoreCatalogInput take_published();

private:
    enum class ActivePhase : uint8_t {
        Idle,
        LoadingMetadata,
        LoadingCheckpoint,
        AcquiringFallback,
        WaitingForCatalog,
        Executing,
        Publishing,
    };

    bool source_current(const ReportArtifactKey &artifact) const;
    bool start_next(uint32_t now_ms);
    bool start_request(ReportArtifactRequest request, uint32_t now_ms);
    bool finish_metadata_load(uint32_t now_ms);
    bool start_known_request(const ReportSignalStoreCatalogRecord *stored,
                             uint32_t now_ms);
    bool start_build(uint32_t now_ms);
    bool finish_checkpoint_load(uint32_t now_ms);
    bool start_execution(uint32_t now_ms);
    bool finish_fallback_acquisition();
    bool finish_execution(uint32_t now_ms);
    bool finish_publication();
    bool retry_active(uint32_t now_ms, uint32_t delay_ms);
    void cancel_active_work();
    void complete_active(OperationOutcome outcome,
                         ReportPlanStatus plan_status,
                         ReportExecutorError executor_error,
                         const char *error = nullptr,
                         ReportSourceId fallback_source =
                             ReportSourceId::Summary);
    void reset_active();

    ReportRequestQueue queue_;
    ReportFallbackAcquisitionService fallback_acquisition_;
    ReportExecutor executor_;
    ReportSignalStoreBuilder builder_;
    ReportSignalStoreService store_;

    StorageBoundedFileLoader metadata_loader_;
    std::shared_ptr<const LargeByteBuffer> previous_metadata_;
    std::shared_ptr<const LargeByteBuffer> previous_checkpoint_;

    std::shared_ptr<const NightCatalog> catalog_;
    std::shared_ptr<const ReportSignalStoreCatalog> store_catalog_;
    ReportSpoolAvailability spool_availability_;
    bool spool_availability_complete_ = false;
    std::shared_ptr<const ReportReadPlan> active_plan_;
    ReportArtifactRequest active_request_;
    ReportEngineCompletion last_completion_;
    ReportSignalStoreCatalogInput published_;
    uint64_t awaited_fallback_identity_ = 0;
    uint32_t active_store_generation_ = 0;
    ActivePhase phase_ = ActivePhase::Idle;
    bool clear_after_fallback_cancel_ = false;
};

}  // namespace aircannect
