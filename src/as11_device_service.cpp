#include "as11_device_service.h"

#include <algorithm>
#include <stdio.h>
#include <time.h>

#include "as11_rpc.h"
#include "board.h"
#ifdef ARDUINO
#include "debug_log.h"
#endif

namespace aircannect {
namespace {

constexpr int64_t ValidUtcMinMs = 1609459200000LL;

bool event_suggests_identity_refresh(const std::string &event) {
    return event == "PowerUp" ||
           event == "SettingsReset" ||
           event == "ResetToDefaultsComplete" ||
           event == "RpcEraseData";
}

bool event_suggests_status_refresh(const std::string &event) {
    return event == "TherapyStarted" ||
           event == "StandbyStarted" ||
           event == "MaskfitStarted" ||
           event == "TestDriveStarted" ||
           event == "CalibrationStarted";
}

bool event_suggests_motor_refresh(const std::string &event) {
    return event == "StandbyStarted" || event == "CooldownStopped";
}

std::string format_utc_ms(int64_t epoch_ms) {
    if (epoch_ms < ValidUtcMinMs) return {};

    const time_t epoch = static_cast<time_t>(epoch_ms / 1000);
    struct tm utc = {};
    gmtime_r(&epoch, &utc);

    char base[25];
    if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return {};
    }

    char out[32];
    snprintf(out, sizeof(out), "%s.%03dZ", base,
             static_cast<int>(epoch_ms % 1000));
    return out;
}

const char *therapy_method(As11TherapyTarget target) {
    switch (target) {
        case As11TherapyTarget::Running: return "EnterTherapy";
        case As11TherapyTarget::Standby: return "EnterStandby";
        case As11TherapyTarget::None:
        default: return nullptr;
    }
}

}  // namespace

bool As11DeviceService::request_healthcheck(RpcRequestPort &rpc,
                                            RpcSource source,
                                            uint32_t now_ms) {
    schedule_initialized_ = true;
    schedule_query(QueryKind::Identity, now_ms, source);
    schedule_query(QueryKind::Runtime, now_ms, source);
    schedule_query(QueryKind::MotorRuntime, now_ms, source);
    schedule_query(QueryKind::Timezone, now_ms, source);
    schedule_query(QueryKind::Clock, now_ms, source);

    if (!query_ticket_.valid()) {
        const QueryKind due = next_due_query(now_ms, false);
        if (due != QueryKind::None) (void)submit_query(rpc, due, now_ms);
    }
    return true;
}

bool As11DeviceService::request_clock_read(RpcRequestPort &rpc,
                                           RpcSource source,
                                           uint32_t now_ms) {
    schedule_initialized_ = true;
    schedule_query(QueryKind::Clock, now_ms, source);

    if (!query_ticket_.valid()) {
        const QueryKind due = next_due_query(now_ms, false);
        if (due != QueryKind::None) (void)submit_query(rpc, due, now_ms);
    }
    return true;
}

OperationSubmission As11DeviceService::request_therapy(
    RpcRequestPort &rpc,
    As11TherapyTarget target,
    RpcSource source,
    uint32_t now_ms) {
    const char *method = therapy_method(target);
    if (!method) return OperationSubmission::rejected();
    if (therapy_ticket_.valid() || state_.therapy_command_pending()) {
        return OperationSubmission::busy();
    }

    RpcRequestCommand command;
    command.method = method;
    command.source = source;
    command.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    command.generation = next_generation();

    const OperationSubmission submitted = rpc.request(command);
    if (!submitted.accepted()) return submitted;

    therapy_ticket_ = submitted.ticket;
    therapy_method_ = method;
    state_.mark_therapy_command_sent(therapy_method_, now_ms);
    note_change();
    return submitted;
}

OperationSubmission As11DeviceService::request_set_datetime_now(
    RpcRequestPort &rpc,
    RpcSource source,
    uint32_t now_ms,
    int64_t utc_ms) {
    if (clock_write_ticket_.valid()) return OperationSubmission::busy();
    if (utc_ms < ValidUtcMinMs) return OperationSubmission::rejected();

    int64_t target_utc_ms = ((utc_ms / 1000) + 1) * 1000;
    int64_t remaining_ms = target_utc_ms - utc_ms;
    if (remaining_ms <=
        static_cast<int64_t>(AC_RPC_SET_DATETIME_APPLY_LEAD_MS +
                             AC_RPC_SET_DATETIME_TARGET_MARGIN_MS)) {
        target_utc_ms += 1000;
        remaining_ms += 1000;
    }

    const std::string target = format_utc_ms(target_utc_ms);
    if (target.empty()) return OperationSubmission::rejected();

    RpcRequestCommand command;
    command.method = "SetDateTime";
    command.params_json = build_set_datetime_params(target);
    command.source = source;
    command.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    command.generation = next_generation();
    command.dispatch_window.enabled = true;
    command.dispatch_window.not_before_ms =
        now_ms + static_cast<uint32_t>(
            remaining_ms - AC_RPC_SET_DATETIME_APPLY_LEAD_MS);
    command.dispatch_window.deadline_ms =
        now_ms + static_cast<uint32_t>(
            remaining_ms + AC_RPC_SET_DATETIME_TARGET_MARGIN_MS);

    const OperationSubmission submitted = rpc.request(command);
    if (!submitted.accepted()) return submitted;

    clock_write_ticket_ = submitted.ticket;
    return submitted;
}

bool As11DeviceService::apply_activity_event_frame(
    const As11EventFrame &frame,
    uint32_t now_ms) {
    const As11TherapyState before_state = state_.therapy_state();
    if (!state_.apply_activity_event_frame(frame, now_ms)) return false;

    const std::string &event = state_.last_activity_event();
    if (event_suggests_identity_refresh(event)) {
        schedule_query(QueryKind::Identity,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
        schedule_query(QueryKind::Runtime,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
        schedule_query(QueryKind::MotorRuntime,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
        schedule_query(QueryKind::Timezone,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
    }

    if (state_.therapy_state() != before_state ||
        event_suggests_status_refresh(event)) {
        schedule_query(QueryKind::Runtime,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
    }

    if (event_suggests_motor_refresh(event) &&
        state_.therapy_state() != As11TherapyState::Running) {
        schedule_query(QueryKind::MotorRuntime,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
    }

    note_change();
    return true;
}

void As11DeviceService::device_reset(RpcRequestPort &rpc, uint32_t now_ms) {
    cancel_ticket(rpc, query_ticket_);
    cancel_ticket(rpc, therapy_ticket_);
    cancel_ticket(rpc, clock_write_ticket_);

    active_query_kind_ = QueryKind::None;
    therapy_method_.clear();
    for (ScheduledQuery &query : queries_) query = {};

    state_.reset();
    schedule_initialized_ = false;
    initialize_schedule(now_ms);
    note_change();
}

void As11DeviceService::poll(RpcRequestPort &rpc,
                             uint32_t now_ms,
                             bool background_suspended) {
    RpcRequestCompletion completion;
    if (query_ticket_.valid() &&
        rpc.take_completion(query_ticket_, completion)) {
        query_ticket_ = {};
        complete_query(completion, now_ms);
        active_query_kind_ = QueryKind::None;
    }

    if (therapy_ticket_.valid() &&
        rpc.take_completion(therapy_ticket_, completion)) {
        therapy_ticket_ = {};
        complete_therapy(completion, now_ms);
        therapy_method_.clear();
    }

    if (clock_write_ticket_.valid() &&
        rpc.take_completion(clock_write_ticket_, completion)) {
        clock_write_ticket_ = {};
        complete_clock_write(completion);
    }

    const bool therapy_pending = state_.therapy_command_pending();
    state_.poll(now_ms);
    if (therapy_pending != state_.therapy_command_pending()) note_change();

    if (!schedule_initialized_) initialize_schedule(now_ms);
    if (query_ticket_.valid()) return;

    const QueryKind due = next_due_query(now_ms, background_suspended);
    if (due != QueryKind::None) (void)submit_query(rpc, due, now_ms);
}

uint32_t As11DeviceService::next_generation() {
    next_generation_++;
    if (next_generation_ == 0) next_generation_++;
    return next_generation_;
}

void As11DeviceService::note_change() {
    revision_++;
    if (revision_ == 0) revision_++;
}

void As11DeviceService::initialize_schedule(uint32_t now_ms) {
    schedule_initialized_ = true;
    schedule_query(QueryKind::Identity,
                   now_ms + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS,
                   RpcSource::Scheduler);
    schedule_query(QueryKind::Runtime,
                   now_ms + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS,
                   RpcSource::Scheduler);
    schedule_query(QueryKind::MotorRuntime,
                   now_ms + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
                       AC_RPC_MIN_TX_INTERVAL_MS,
                   RpcSource::Scheduler);
    schedule_query(QueryKind::Timezone,
                   now_ms + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
                       (AC_RPC_MIN_TX_INTERVAL_MS * 2),
                   RpcSource::Scheduler);
    schedule_query(QueryKind::Clock,
                   now_ms + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
                       AC_RPC_DEFAULT_TIMEOUT_MS,
                   RpcSource::Scheduler);
}

void As11DeviceService::schedule_query(QueryKind kind,
                                       uint32_t due_ms,
                                       RpcSource source) {
    if (kind == QueryKind::None || kind == QueryKind::Count) return;

    ScheduledQuery &query = queries_[query_index(kind)];
    if (!query.scheduled ||
        static_cast<int32_t>(due_ms - query.due_ms) < 0) {
        query.due_ms = due_ms;
    }
    if (!query.scheduled ||
        (background_source(query.source) && !background_source(source))) {
        query.source = source;
    }
    query.scheduled = true;
}

As11DeviceService::QueryKind As11DeviceService::next_due_query(
    uint32_t now_ms,
    bool background_suspended) const {
    QueryKind selected = QueryKind::None;
    uint32_t selected_due_ms = 0;

    for (size_t i = query_index(QueryKind::Identity);
         i < query_index(QueryKind::Count); ++i) {
        const ScheduledQuery &query = queries_[i];
        if (!query.scheduled ||
            static_cast<int32_t>(now_ms - query.due_ms) < 0) {
            continue;
        }
        if (background_suspended && background_source(query.source)) {
            continue;
        }
        if (selected == QueryKind::None ||
            static_cast<int32_t>(query.due_ms - selected_due_ms) < 0) {
            selected = static_cast<QueryKind>(i);
            selected_due_ms = query.due_ms;
        }
    }
    return selected;
}

bool As11DeviceService::submit_query(RpcRequestPort &rpc,
                                     QueryKind kind,
                                     uint32_t now_ms) {
    ScheduledQuery &query = queries_[query_index(kind)];

    RpcRequestCommand command;
    command.method = kind == QueryKind::Clock ? "GetDateTime" : "Get";
    command.params_json = query_params(kind);
    command.source = query.source;
    command.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    command.generation = next_generation();

    const OperationSubmission submitted = rpc.request(command);
    if (!submitted.accepted()) {
        query.due_ms = now_ms + QueryRetryMs;
        return false;
    }

    query = {};
    query_ticket_ = submitted.ticket;
    active_query_kind_ = kind;
    return true;
}

void As11DeviceService::complete_query(
    const RpcRequestCompletion &completion,
    uint32_t now_ms) {
    if (!completion_succeeded(completion)) {
        schedule_query(active_query_kind_, now_ms + QueryRetryMs,
                       RpcSource::Scheduler);
        return;
    }

    const As11TherapyState before_state = state_.therapy_state();
    const bool had_pending_therapy = state_.therapy_command_pending();
    bool applied = false;
    if (active_query_kind_ == QueryKind::Clock) {
        applied = state_.apply_datetime_response(
            completion.payload, now_ms, completion.dispatch_utc_ms,
            completion.response_utc_ms, completion.dispatch_ms,
            completion.response_ms);
    } else {
        applied = state_.apply_status_get_response(completion.payload, now_ms);
    }

    if (!applied) {
        schedule_query(active_query_kind_, now_ms + QueryRetryMs,
                       RpcSource::Scheduler);
        return;
    }
    note_change();

    switch (active_query_kind_) {
        case QueryKind::Identity:
            break;
        case QueryKind::Runtime:
            if (before_state == As11TherapyState::Running &&
                state_.therapy_state() == As11TherapyState::Standby) {
                schedule_query(
                    QueryKind::MotorRuntime,
                    now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                    RpcSource::Scheduler);
            }
            if (state_.therapy_state() == As11TherapyState::Running) {
                schedule_query(
                    QueryKind::MotorRuntime,
                    now_ms + AC_AS11_MOTOR_RUNTIME_POLL_INTERVAL_MS,
                    RpcSource::Scheduler);
            }
            schedule_query(
                QueryKind::Runtime,
                now_ms + (had_pending_therapy &&
                                  state_.therapy_command_pending()
                              ? AC_AS11_THERAPY_STATUS_POLL_INTERVAL_MS
                              : AC_AS11_STATUS_POLL_INTERVAL_MS),
                RpcSource::Scheduler);
            break;
        case QueryKind::MotorRuntime:
            if (state_.therapy_state() == As11TherapyState::Running) {
                schedule_query(
                    QueryKind::MotorRuntime,
                    now_ms + AC_AS11_MOTOR_RUNTIME_POLL_INTERVAL_MS,
                    RpcSource::Scheduler);
            }
            break;
        case QueryKind::Timezone:
            schedule_query(QueryKind::Timezone,
                           now_ms + AC_AS11_TIMEZONE_POLL_INTERVAL_MS,
                           RpcSource::Scheduler);
            break;
        case QueryKind::Clock:
            schedule_query(QueryKind::Clock,
                           now_ms + AC_AS11_CLOCK_POLL_INTERVAL_MS,
                           RpcSource::Scheduler);
            break;
        case QueryKind::None:
        case QueryKind::Count:
            break;
    }
}

void As11DeviceService::complete_therapy(
    const RpcRequestCompletion &completion,
    uint32_t now_ms) {
    if (completion.cause == RpcCompletionCause::Response) {
        state_.mark_therapy_command_response(
            therapy_method_, !completion_succeeded(completion), now_ms);
    } else if (completion.cause == RpcCompletionCause::Timeout) {
        state_.mark_therapy_command_timeout(therapy_method_, now_ms);
    } else {
        state_.clear_pending_therapy_command(
            completion.reason.empty() ? "cancelled"
                                      : completion.reason.c_str(),
            now_ms);
    }
    note_change();

    if (completion_succeeded(completion)) {
        schedule_query(QueryKind::Runtime,
                       now_ms + AC_AS11_THERAPY_STATUS_POLL_DELAY_MS,
                       RpcSource::Scheduler);
    }
}

void As11DeviceService::complete_clock_write(
    const RpcRequestCompletion &completion) {
    if (completion_succeeded(completion)) return;

#ifdef ARDUINO
    Log::logf(CAT_GENERAL, LOG_WARN,
              "[TIME] AS11 SetDateTime failed reason=%s\n",
              completion.reason.empty() ? "rpc_error"
                                        : completion.reason.c_str());
#else
    (void)completion;
#endif
}

void As11DeviceService::cancel_ticket(RpcRequestPort &rpc,
                                      OperationTicket &ticket) {
    if (!ticket.valid()) return;

    (void)rpc.cancel(ticket);
    RpcRequestCompletion completion;
    (void)rpc.take_completion(ticket, completion);
    ticket = {};
}

size_t As11DeviceService::query_index(QueryKind kind) {
    return static_cast<size_t>(kind);
}

bool As11DeviceService::background_source(RpcSource source) {
    return source == RpcSource::Scheduler || source == RpcSource::Internal;
}

bool As11DeviceService::completion_succeeded(
    const RpcRequestCompletion &completion) {
    return completion.cause == RpcCompletionCause::Response &&
           completion.outcome.disposition ==
               OperationDisposition::Succeeded &&
           !completion.response_error;
}

const char *As11DeviceService::query_params(QueryKind kind) {
    switch (kind) {
        case QueryKind::Identity: return as11_identity_get_params_json();
        case QueryKind::Runtime: return as11_runtime_get_params_json();
        case QueryKind::MotorRuntime:
            return as11_motor_runtime_get_params_json();
        case QueryKind::Timezone: return as11_timezone_get_params_json();
        case QueryKind::Clock:
        case QueryKind::None:
        case QueryKind::Count:
        default: return "";
    }
}

}  // namespace aircannect
