#pragma once

#include <memory>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "report_read_plan.h"

namespace aircannect {

static constexpr int64_t REPORT_SOURCE_EDGE_TOLERANCE_MS = 15000;

struct ReportPlanRequest {
    ReportArtifactKey artifact;
    uint32_t signal_mask = 0;
    uint8_t event_mask = 0;

    bool valid() const;
};

enum class ReportPlanStatus : uint8_t {
    Ready,
    InvalidRequest,
    NightMissing,
    StaleRevision,
    InvalidCatalog,
    AllocationFailed,
};

struct ReportPlanResult {
    ReportPlanStatus status = ReportPlanStatus::InvalidRequest;
    std::shared_ptr<const ReportReadPlan> plan;

    bool ready() const {
        return status == ReportPlanStatus::Ready && plan != nullptr;
    }
};

class ReportPlanner {
public:
    static ReportPlanResult build(
        const ReportPlanRequest &request,
        std::shared_ptr<const NightCatalog> catalog);

    static std::shared_ptr<const LargeByteBuffer> capture_progress(
        const ReportReadPlan &prepared,
        int64_t closed_before_ms);

    // Resume a full plan returned by build(), retaining the saved byte owner.
    static ReportPlanResult resume(
        std::shared_ptr<const ReportReadPlan> full,
        std::shared_ptr<const LargeByteBuffer> progress);
};

}  // namespace aircannect
