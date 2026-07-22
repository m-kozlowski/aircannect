#pragma once

#include <stdint.h>

#include "operation_outcome.h"
#include "report_sources.h"
#include "report_spool_types.h"
#include "storage_path.h"

namespace aircannect {

struct ReportSpoolFetchCommand {
    ReportSourceId source = ReportSourceId::Summary;
    int64_t from_ms = 0;
    uint32_t generation = 0;

    bool valid() const {
        return source != ReportSourceId::Summary && from_ms > 0 &&
               generation != 0;
    }
};

struct ReportSpoolFetchCompletion {
    OperationTicket ticket;
    OperationOutcome outcome = OperationOutcome::failed();
    ReportSpoolResult result;
    char error[AC_STORAGE_ERROR_MAX] = {};

    void clear();
    void move_from(ReportSpoolFetchCompletion &other);
};

class ReportSpoolPort {
public:
    virtual ~ReportSpoolPort() = default;

    virtual OperationSubmission request_fetch(
        const ReportSpoolFetchCommand &command) = 0;
    virtual bool cancel(OperationTicket ticket) = 0;
    virtual bool take_completion(
        OperationTicket ticket,
        ReportSpoolFetchCompletion &completion) = 0;
};

}  // namespace aircannect
