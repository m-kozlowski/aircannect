#include "report_summary_acquisition.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {
namespace {

constexpr int64_t SUMMARY_FROM_MS = 946684800000LL;

void copy_error(char *target, size_t target_size, const char *error) {
    if (!target || target_size == 0) return;
    snprintf(target, target_size, "%s", error ? error : "");
}

}  // namespace

void ReportSummaryAcquisition::begin(ReportSpoolPort &spool_port) {
    spool_port_ = &spool_port;
}

OperationAdmission ReportSummaryAcquisition::request(uint32_t generation) {
    if (!spool_port_ || generation == 0) return OperationAdmission::Rejected;
    if (active()) return OperationAdmission::Busy;

    ReportSpoolFetchCommand command;
    command.source = ReportSourceId::Summary;
    command.from_ms = SUMMARY_FROM_MS;
    command.generation = generation;

    const OperationSubmission submission = spool_port_->request_fetch(command);
    if (!submission.accepted()) return submission.admission;

    ticket_ = submission.ticket;
    cancel_requested_ = false;
    status_.state = ReportSummaryAcquisitionState::Waiting;
    status_.generation = generation;
    status_.error[0] = '\0';
    return OperationAdmission::Accepted;
}

bool ReportSummaryAcquisition::poll() {
    if (!active() || !spool_port_ || !ticket_.valid()) return false;

    ReportSpoolFetchRound round;
    if (spool_port_->take_round(ticket_, round)) {
        fail("summary_round_unexpected");
        return true;
    }

    ReportSpoolFetchCompletion completion;
    if (!spool_port_->take_completion(ticket_, completion)) return false;

    ticket_ = {};
    if (cancel_requested_ ||
        completion.outcome.disposition == OperationDisposition::Cancelled) {
        cancel_requested_ = false;
        status_.state = ReportSummaryAcquisitionState::Idle;
        status_.error[0] = '\0';
        return true;
    }
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        fail(completion.error[0] ? completion.error
                                 : "summary_fetch_failed");
        return true;
    }

    char error[AC_STORAGE_ERROR_MAX] = {};
    std::shared_ptr<const NightCatalogSummarySnapshot> parsed =
        NightCatalogSummarySnapshot::parse(completion.result,
                                           error,
                                           sizeof(error));
    if (!parsed) {
        fail(error[0] ? error : "summary_parse_failed");
        return true;
    }

    snapshot_ = std::move(parsed);
    status_.state = ReportSummaryAcquisitionState::Ready;
    status_.records = snapshot_->size();
    status_.error[0] = '\0';
    return true;
}

void ReportSummaryAcquisition::cancel() {
    if (!active() || !spool_port_ || !ticket_.valid()) return;

    cancel_requested_ = true;
    (void)spool_port_->cancel(ticket_);
}

void ReportSummaryAcquisition::seed(
    std::shared_ptr<const NightCatalogSummarySnapshot> snapshot) {
    if (!snapshot) return;

    snapshot_ = std::move(snapshot);
    status_.records = snapshot_->size();
    if (!active()) {
        status_.state = ReportSummaryAcquisitionState::Ready;
        status_.error[0] = '\0';
    }
}

void ReportSummaryAcquisition::fail(const char *error) {
    ticket_ = {};
    cancel_requested_ = false;
    status_.state = ReportSummaryAcquisitionState::Error;
    copy_error(status_.error, sizeof(status_.error), error);
}

}  // namespace aircannect
