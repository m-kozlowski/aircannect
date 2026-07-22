#include "report_spool_service.h"

#include <stdio.h>
#include <string>
#include <time.h>
#include <utility>

#include "board_report.h"
#include "report_sources.h"

namespace aircannect {
namespace {

bool format_utc_ms_iso(int64_t time_ms, std::string &out) {
    if (time_ms <= 0) return false;

    const time_t seconds = static_cast<time_t>(time_ms / 1000);
    struct tm value;
    if (!gmtime_r(&seconds, &value)) return false;

    char text[32] = {};
    if (!strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &value)) {
        return false;
    }

    out = text;
    out += ".000Z";
    return true;
}

}  // namespace

ReportSpoolService::~ReportSpoolService() {
#ifdef ARDUINO
    if (mutex_) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
#endif
}

bool ReportSpoolService::begin() {
    if (initialized_) return true;

#ifdef ARDUINO
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
#endif

    initialized_ = true;
    return true;
}

OperationSubmission ReportSpoolService::request_fetch(
    const ReportSpoolFetchCommand &command) {
    if (!initialized_ || !command.valid() || !lock()) {
        return OperationSubmission::rejected();
    }
    if (queued_ || active_ticket_.valid() || round_ready_ ||
        completion_ready_) {
        unlock();
        return OperationSubmission::busy();
    }

    uint32_t ticket_id = next_ticket_id_++;
    if (ticket_id == 0) ticket_id = next_ticket_id_++;

    queued_command_ = command;
    queued_ticket_ = {ticket_id, command.generation};
    queued_ = true;
    const OperationTicket ticket = queued_ticket_;
    unlock();
    return OperationSubmission::accepted(ticket);
}

bool ReportSpoolService::cancel(OperationTicket ticket) {
    if (!initialized_ || !ticket.valid() || !lock()) return false;

    if (queued_ && queued_ticket_ == ticket) {
        queued_ = false;
        queued_command_ = {};
        queued_ticket_ = {};
        completion_.clear();
        completion_.ticket = ticket;
        completion_.outcome = OperationOutcome::cancelled();
        completion_ready_ = true;
        unlock();
        return true;
    }
    if (active_ticket_ == ticket && !cancel_ticket_.valid()) {
        cancel_ticket_ = ticket;
        unlock();
        return true;
    }

    unlock();
    return false;
}

bool ReportSpoolService::take_round(OperationTicket ticket,
                                    ReportSpoolFetchRound &round) {
    if (!initialized_ || !ticket.valid() || !lock()) return false;
    if (!round_ready_ || round_.ticket != ticket) {
        unlock();
        return false;
    }

    round.move_from(round_);
    round_ready_ = false;
    unlock();
    return true;
}

bool ReportSpoolService::take_completion(
    OperationTicket ticket,
    ReportSpoolFetchCompletion &completion) {
    if (!initialized_ || !ticket.valid() || !lock()) return false;
    if (!completion_ready_ || completion_.ticket != ticket) {
        unlock();
        return false;
    }

    completion.move_from(completion_);
    completion_ready_ = false;
    unlock();
    return true;
}

bool ReportSpoolService::enqueue_notification(const char *payload,
                                              size_t payload_len) {
    return initialized_ &&
           runtime_.enqueue_notification(payload, payload_len);
}

bool ReportSpoolService::poll(bool transport_backpressure_active,
                              uint32_t rx_queue_full_alerts) {
    if (!initialized_) return false;

    OperationTicket cancelled;
    if (take_cancel_request(cancelled)) {
        runtime_.reset();
        clear_published_round(cancelled);
        publish_completion(cancelled,
                           OperationOutcome::cancelled(),
                           nullptr,
                           nullptr);
        return true;
    }

    if (round_waiting()) return true;

    ReportSpoolFetchCommand command;
    OperationTicket ticket;
    if (take_queued(command, ticket)) {
        const ReportSourceDef *source = report_source_def(command.source);
        std::string from_dt;
        if (!source || !source->spool_type || !source->spool_type[0] ||
            !format_utc_ms_iso(command.from_ms, from_dt)) {
            publish_completion(ticket,
                               OperationOutcome::failed(),
                               nullptr,
                               "spool_request_invalid");
            return true;
        }

        SpoolClientRequest request;
        request.spool_type = source->spool_type;
        request.from_dt = std::move(from_dt);
        request.max_size = AC_REPORT_CACHE_SPOOL_ROUND_BYTES;
        request.fragment_max = AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES;
        request.max_notifications =
            AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL;
        request.max_rounds = 128;
        request.pace_on_backpressure = true;
        request.stream_rounds = true;
        if (!runtime_.begin(request)) {
            publish_completion(ticket,
                               OperationOutcome::failed(),
                               nullptr,
                               "spool_start_failed");
            return true;
        }
    }

    if (!runtime_.active() && !runtime_.complete() && !runtime_.failed()) {
        runtime_.observe_idle(rx_queue_full_alerts);
        return false;
    }

    const bool drained = runtime_.drain_notification();
    runtime_.poll(transport_backpressure_active, rx_queue_full_alerts);

    if (publish_completed_round()) return true;

    if (runtime_.complete()) {
        ReportSpoolResult result;
        runtime_.move_result_to(result);
        OperationTicket completed;
        if (lock()) {
            completed = active_ticket_;
            active_ticket_ = {};
            unlock();
        }
        runtime_.reset();
        publish_completion(completed,
                           OperationOutcome::succeeded(),
                           &result,
                           nullptr);
        return true;
    }
    if (runtime_.failed()) {
        const SpoolClientStatus status = runtime_.status();
        OperationTicket completed;
        if (lock()) {
            completed = active_ticket_;
            active_ticket_ = {};
            unlock();
        }
        runtime_.reset();
        publish_completion(completed,
                           OperationOutcome::failed(),
                           nullptr,
                           status.error.empty()
                               ? "spool_fetch_failed"
                               : status.error.c_str());
        return true;
    }

    return drained || runtime_.active();
}

bool ReportSpoolService::active() const {
    if (!initialized_ || !lock()) return false;
    const bool value = queued_ || active_ticket_.valid() || round_ready_;
    unlock();
    return value;
}

bool ReportSpoolService::lock(uint32_t timeout_ms) const {
#ifdef ARDUINO
    return mutex_ &&
           xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
#else
    (void)timeout_ms;
    return true;
#endif
}

void ReportSpoolService::unlock() const {
#ifdef ARDUINO
    if (mutex_) xSemaphoreGive(mutex_);
#endif
}

bool ReportSpoolService::take_queued(ReportSpoolFetchCommand &command,
                                     OperationTicket &ticket) {
    if (!lock()) return false;
    if (!queued_ || active_ticket_.valid()) {
        unlock();
        return false;
    }

    command = queued_command_;
    ticket = queued_ticket_;
    queued_command_ = {};
    queued_ticket_ = {};
    queued_ = false;
    active_ticket_ = ticket;
    unlock();
    return true;
}

bool ReportSpoolService::take_cancel_request(OperationTicket &ticket) {
    if (!lock()) return false;
    if (!cancel_ticket_.valid() || cancel_ticket_ != active_ticket_) {
        unlock();
        return false;
    }

    ticket = cancel_ticket_;
    cancel_ticket_ = {};
    active_ticket_ = {};
    unlock();
    return true;
}

bool ReportSpoolService::round_waiting() const {
    if (!lock()) return true;
    const bool waiting = round_ready_;
    unlock();
    return waiting;
}

bool ReportSpoolService::publish_completed_round() {
    if (!lock()) return false;
    if (round_ready_ || !active_ticket_.valid()) {
        unlock();
        return false;
    }

    ReportSpoolResult result;
    if (!runtime_.take_completed_round(result)) {
        unlock();
        return false;
    }

    round_.clear();
    round_.ticket = active_ticket_;
    round_.result.move_from(result);
    round_ready_ = true;
    unlock();
    return true;
}

void ReportSpoolService::clear_published_round(OperationTicket ticket) {
    if (!ticket.valid() || !lock()) return;
    if (round_ready_ && round_.ticket == ticket) {
        round_.clear();
        round_ready_ = false;
    }
    unlock();
}

void ReportSpoolService::publish_completion(
    OperationTicket ticket,
    OperationOutcome outcome,
    ReportSpoolResult *result,
    const char *error) {
    if (!ticket.valid() || !lock()) return;

    completion_.clear();
    completion_.ticket = ticket;
    completion_.outcome = outcome;
    if (result) completion_.result.move_from(*result);
    if (error) {
        snprintf(completion_.error,
                 sizeof(completion_.error),
                 "%s",
                 error);
    }
    if (active_ticket_ == ticket) active_ticket_ = {};
    if (cancel_ticket_ == ticket) cancel_ticket_ = {};
    completion_ready_ = true;
    unlock();
}

}  // namespace aircannect
