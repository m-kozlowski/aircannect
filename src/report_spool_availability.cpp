#include "report_spool_availability.h"

#include <stdio.h>

#include "report_parser.h"

namespace aircannect {
namespace {

constexpr int64_t PROBE_FROM_MS = 946684800000LL;

constexpr ReportSourceId PROBED_SOURCES[] = {
    ReportSourceId::RespiratoryEvents,
    ReportSourceId::TherapyOneMinute,
    ReportSourceId::RespiratoryFlow6p25Hz,
    ReportSourceId::MaskPressure6p25Hz,
};

size_t source_index(ReportSourceId source) {
    const size_t index = static_cast<size_t>(source);
    const size_t count =
        static_cast<size_t>(ReportSourceId::OximetryOneSecond) + 1;
    return index < count ? index : SIZE_MAX;
}

void copy_error(char *target, size_t target_size, const char *error) {
    if (!target || target_size == 0) return;
    snprintf(target, target_size, "%s", error ? error : "");
}

struct OldestTimestamp {
    int64_t value = 0;
};

}  // namespace

void ReportSpoolAvailability::clear() {
    for (ReportSpoolBoundary &boundary : boundaries_) boundary = {};
}

void ReportSpoolAvailability::mark_available(ReportSourceId source,
                                             int64_t oldest_ms) {
    const size_t index = source_index(source);
    if (index == SIZE_MAX || oldest_ms <= 0) return;

    boundaries_[index].state = ReportSpoolBoundaryState::Available;
    boundaries_[index].oldest_ms = oldest_ms;
}

void ReportSpoolAvailability::mark_empty(ReportSourceId source) {
    const size_t index = source_index(source);
    if (index == SIZE_MAX) return;

    boundaries_[index].state = ReportSpoolBoundaryState::Empty;
    boundaries_[index].oldest_ms = 0;
}

ReportSpoolBoundary ReportSpoolAvailability::boundary(
    ReportSourceId source) const {
    const size_t index = source_index(source);
    return index == SIZE_MAX ? ReportSpoolBoundary{} : boundaries_[index];
}

bool ReportSpoolAvailability::excludes(ReportSourceId source,
                                       int64_t until_ms) const {
    if (until_ms <= 0) return false;

    const ReportSpoolBoundary value = boundary(source);
    return value.state == ReportSpoolBoundaryState::Empty ||
           (value.state == ReportSpoolBoundaryState::Available &&
            until_ms <= value.oldest_ms);
}

void ReportSpoolAvailabilityProbe::begin(ReportSpoolPort &spool_port) {
    spool_port_ = &spool_port;
}

OperationAdmission ReportSpoolAvailabilityProbe::request(
    uint32_t generation) {
    if (!spool_port_ || generation == 0) return OperationAdmission::Rejected;
    if (status_.active()) return OperationAdmission::Busy;

    ticket_ = {};
    availability_.clear();
    status_ = {};
    status_.state = ReportSpoolAvailabilityProbeState::Fetching;
    status_.generation = generation;
    status_.source = PROBED_SOURCES[0];
    source_index_ = 0;
    cancel_requested_ = false;
    return OperationAdmission::Accepted;
}

bool ReportSpoolAvailabilityProbe::poll() {
    if (!status_.active() || !spool_port_) return false;

    if (!ticket_.valid()) return submit_current();
    return finish_current();
}

void ReportSpoolAvailabilityProbe::cancel() {
    if (status_.active() && spool_port_ && ticket_.valid()) {
        cancel_requested_ = true;
        (void)spool_port_->cancel(ticket_);
        return;
    }

    ticket_ = {};
    status_ = {};
    source_index_ = 0;
    cancel_requested_ = false;
}

bool ReportSpoolAvailabilityProbe::capture_oldest(
    void *context,
    const ReportParsedChunk &chunk) {
    OldestTimestamp *oldest = static_cast<OldestTimestamp *>(context);
    if (!oldest || chunk.start_ms <= 0) return false;

    if (oldest->value == 0 || chunk.start_ms < oldest->value) {
        oldest->value = chunk.start_ms;
    }
    return true;
}

bool ReportSpoolAvailabilityProbe::submit_current() {
    if (source_index_ >= sizeof(PROBED_SOURCES) / sizeof(PROBED_SOURCES[0])) {
        status_.state = status_.sources_known == status_.sources_completed
            ? ReportSpoolAvailabilityProbeState::Ready
            : ReportSpoolAvailabilityProbeState::Incomplete;
        status_.source = ReportSourceId::Summary;
        return true;
    }

    ReportSpoolFetchCommand command;
    command.kind = ReportSpoolFetchKind::AvailabilityProbe;
    command.source = PROBED_SOURCES[source_index_];
    command.from_ms = PROBE_FROM_MS;
    command.generation = status_.generation;

    const OperationSubmission submission = spool_port_->request_fetch(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        advance(false, "spool_probe_request_rejected");
        return true;
    }

    ticket_ = submission.ticket;
    status_.source = command.source;
    return true;
}

bool ReportSpoolAvailabilityProbe::finish_current() {
    ReportSpoolFetchRound round;
    if (spool_port_->take_round(ticket_, round)) {
        round.clear();
        advance(false, "spool_probe_round_unexpected");
        return true;
    }

    ReportSpoolFetchCompletion completion;
    if (!spool_port_->take_completion(ticket_, completion)) return false;

    ticket_ = {};
    if (cancel_requested_ ||
        completion.outcome.disposition == OperationDisposition::Cancelled) {
        cancel_requested_ = false;
        status_.state = ReportSpoolAvailabilityProbeState::Idle;
        status_.source = ReportSourceId::Summary;
        completion.clear();
        return true;
    }

    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        advance(false,
                completion.error[0]
                    ? completion.error
                    : "spool_probe_fetch_failed");
        completion.clear();
        return true;
    }

    int64_t oldest_ms = 0;
    bool empty = false;
    char error[AC_STORAGE_ERROR_MAX] = {};
    if (!parse_current(completion.result,
                       oldest_ms,
                       empty,
                       error,
                       sizeof(error))) {
        advance(false, error[0] ? error : "spool_probe_parse_failed");
        completion.clear();
        return true;
    }

    const ReportSourceId source = PROBED_SOURCES[source_index_];
    if (empty) {
        availability_.mark_empty(source);
    } else {
        availability_.mark_available(source, oldest_ms);
    }
    advance(true);
    completion.clear();
    return true;
}

bool ReportSpoolAvailabilityProbe::parse_current(
    ReportSpoolResult &result,
    int64_t &oldest_ms,
    bool &empty,
    char *error,
    size_t error_len) {
    oldest_ms = 0;
    empty = false;

    const ReportSourceId source = PROBED_SOURCES[source_index_];
    const ReportSourceDef *definition = report_source_def(source);
    if (!definition || !definition->spool_type ||
        result.spool_type != definition->spool_type) {
        copy_error(error, error_len, "wrong_spool_type");
        return false;
    }
    if (!result.complete) {
        copy_error(error, error_len, "spool_incomplete");
        return false;
    }
    if (!result.sha_ok) {
        copy_error(error, error_len, "spool_hash_failed");
        return false;
    }
    if (!result.payload.data() || result.payload.size() == 0) {
        empty = true;
        copy_error(error, error_len, "");
        return true;
    }

    // A one-round probe intentionally stops before following the next address.
    result.truncated = false;
    OldestTimestamp oldest;
    const bool parsed = source == ReportSourceId::RespiratoryEvents
        ? report_parse_event_spool(result,
                                   source,
                                   capture_oldest,
                                   &oldest,
                                   error,
                                   error_len)
        : report_parse_series_spool(result,
                                    source,
                                    capture_oldest,
                                    &oldest,
                                    error,
                                    error_len);
    if (!parsed) return false;

    if (oldest.value <= 0) {
        empty = true;
    } else {
        oldest_ms = oldest.value;
    }
    copy_error(error, error_len, "");
    return true;
}

void ReportSpoolAvailabilityProbe::advance(bool known, const char *error) {
    ticket_ = {};
    status_.sources_completed++;
    if (known) {
        status_.sources_known++;
    } else if (!status_.error[0]) {
        copy_error(status_.error, sizeof(status_.error), error);
    }

    source_index_++;
    if (source_index_ < sizeof(PROBED_SOURCES) / sizeof(PROBED_SOURCES[0])) {
        status_.source = PROBED_SOURCES[source_index_];
        return;
    }

    status_.state = status_.sources_known == status_.sources_completed
        ? ReportSpoolAvailabilityProbeState::Ready
        : ReportSpoolAvailabilityProbeState::Incomplete;
    status_.source = ReportSourceId::Summary;
}

}  // namespace aircannect
