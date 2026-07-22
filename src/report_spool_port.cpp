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

void ReportSpoolFetchRound::clear() {
    ticket = {};
    result.clear();
}

void ReportSpoolFetchRound::move_from(ReportSpoolFetchRound &other) {
    if (this == &other) return;

    clear();
    ticket = other.ticket;
    result.move_from(other.result);
    other.clear();
}

}  // namespace aircannect
