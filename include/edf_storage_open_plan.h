#pragma once

#include <stdint.h>

#include "edf_file_resume.h"

namespace aircannect {

struct EdfStorageOpenPlanRequest {
    bool annotation = false;
    bool recording_start_requested = false;
    uint32_t requested_record_count = 0;
    EdfResumeDecision resume;
};

struct EdfStorageOpenPlan {
    bool resume_existing = false;
    bool patch_header_record_count = false;
    bool write_recording_start = false;
    uint32_t record_count = 0;
};

EdfStorageOpenPlan edf_storage_plan_open(
    const EdfStorageOpenPlanRequest &request);

}  // namespace aircannect
