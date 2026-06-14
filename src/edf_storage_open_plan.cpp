#include "edf_storage_open_plan.h"

namespace aircannect {

EdfStorageOpenPlan edf_storage_plan_open(
    const EdfStorageOpenPlanRequest &request) {
    EdfStorageOpenPlan plan;
    if (request.resume.status == EdfResumeStatus::Ok) {
        plan.resume_existing = true;
        plan.patch_header_record_count =
            !request.resume.header_record_count_matches;
        plan.record_count = request.resume.record_count;
        return plan;
    }

    plan.record_count = request.requested_record_count;
    plan.write_recording_start =
        request.annotation && request.recording_start_requested;
    return plan;
}

}  // namespace aircannect
