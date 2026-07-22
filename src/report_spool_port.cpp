#include "report_spool_port.h"

#include <string.h>

namespace aircannect {

void ReportSpoolFetchCompletion::clear() {
    ticket = {};
    outcome = OperationOutcome::failed();
    result.clear();
    error[0] = '\0';
}

void ReportSpoolFetchCompletion::move_from(
    ReportSpoolFetchCompletion &other) {
    if (this == &other) return;

    clear();
    ticket = other.ticket;
    outcome = other.outcome;
    result.move_from(other.result);
    memcpy(error, other.error, sizeof(error));
    other.clear();
}

}  // namespace aircannect
