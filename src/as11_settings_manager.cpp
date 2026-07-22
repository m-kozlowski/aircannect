#include "as11_settings_manager.h"

#include "board.h"

namespace aircannect {

bool As11SettingsManager::request_refresh(RpcRequestPort &rpc,
                                          RpcSource source,
                                          uint32_t now_ms) {
    if (refresh_ticket_.valid()) return true;

    if (submit_refresh(rpc, source)) return true;

    schedule_refresh(source, now_ms);
    return false;
}

OperationSubmission As11SettingsManager::write(
    RpcRequestPort &rpc,
    const std::string &params_json,
    RpcSource source,
    uint32_t now_ms) {
    if (write_ticket_.valid()) return OperationSubmission::busy();

    RpcRequestCommand command;
    command.method = "Set";
    command.params_json = params_json;
    command.source = source;
    command.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    command.generation = next_generation();

    const OperationSubmission submitted = rpc.request(command);
    if (!submitted.accepted()) return submitted;

    write_ticket_ = submitted.ticket;
    write_source_ = source;
    (void)state_.note_set_request(params_json, now_ms);
    note_change();
    return submitted;
}

void As11SettingsManager::invalidate(RpcRequestPort &rpc,
                                     RpcSource source,
                                     uint32_t now_ms) {
    note_change();
    if (refresh_ticket_.valid()) {
        refresh_again_pending_ = true;
        if (background_source(refresh_source_) &&
            !background_source(source)) {
            refresh_source_ = source;
        }
        return;
    }

    (void)request_refresh(rpc, source, now_ms);
}

void As11SettingsManager::device_reset(RpcRequestPort &rpc) {
    if (write_ticket_.valid()) (void)rpc.cancel(write_ticket_);
    if (refresh_ticket_.valid()) (void)rpc.cancel(refresh_ticket_);

    state_.clear();
    refresh_retry_pending_ = false;
    refresh_again_pending_ = false;
    next_refresh_retry_ms_ = 0;
    note_change();
}

void As11SettingsManager::poll(RpcRequestPort &rpc,
                               uint32_t now_ms,
                               bool suspended) {
    RpcRequestCompletion completion;
    if (write_ticket_.valid() &&
        rpc.take_completion(write_ticket_, completion)) {
        write_ticket_ = {};
        complete_write(rpc, completion, now_ms);
    }

    if (refresh_ticket_.valid() &&
        rpc.take_completion(refresh_ticket_, completion)) {
        refresh_ticket_ = {};
        complete_refresh(completion, now_ms);
    }

    if (state_.expire_pending(now_ms, ReadbackTimeoutMs)) note_change();

    if (suspended || refresh_ticket_.valid() || !refresh_retry_pending_) {
        return;
    }
    if (static_cast<int32_t>(now_ms - next_refresh_retry_ms_) < 0) return;

    const RpcSource source = refresh_source_;
    if (!submit_refresh(rpc, source)) {
        schedule_refresh(source, now_ms);
    }
}

bool As11SettingsManager::refresh_pending() const {
    return refresh_ticket_.valid() || refresh_retry_pending_ ||
           refresh_again_pending_;
}

uint32_t As11SettingsManager::next_generation() {
    next_generation_++;
    if (next_generation_ == 0) next_generation_++;
    return next_generation_;
}

void As11SettingsManager::note_change() {
    revision_++;
    if (revision_ == 0) revision_++;
}

bool As11SettingsManager::submit_refresh(RpcRequestPort &rpc,
                                         RpcSource source) {
    RpcRequestCommand command;
    command.method = "Get";
    command.params_json = as11_settings_get_params_json();
    command.source = source;
    command.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    command.generation = next_generation();

    const OperationSubmission submitted = rpc.request(command);
    if (!submitted.accepted()) return false;

    refresh_ticket_ = submitted.ticket;
    refresh_source_ = source;
    refresh_retry_pending_ = false;
    next_refresh_retry_ms_ = 0;
    note_change();
    return true;
}

void As11SettingsManager::schedule_refresh(RpcSource source,
                                           uint32_t now_ms,
                                           uint32_t delay_ms) {
    if (!refresh_retry_pending_ ||
        (background_source(refresh_source_) && !background_source(source))) {
        refresh_source_ = source;
    }

    refresh_retry_pending_ = true;
    next_refresh_retry_ms_ = now_ms + delay_ms;
    note_change();
}

void As11SettingsManager::complete_write(
    RpcRequestPort &rpc,
    const RpcRequestCompletion &completion,
    uint32_t now_ms) {
    if (completion.cause == RpcCompletionCause::Response) {
        state_.note_set_response(!completion_succeeded(completion), now_ms);
    } else {
        const char *reason = completion.reason.empty()
            ? "cancelled"
            : completion.reason.c_str();
        state_.note_set_cancelled(reason, now_ms);
    }
    note_change();

    if (!completion_succeeded(completion)) return;

    if (refresh_ticket_.valid()) {
        refresh_again_pending_ = true;
        if (background_source(refresh_source_) &&
            !background_source(write_source_)) {
            refresh_source_ = write_source_;
        }
        return;
    }

    (void)request_refresh(rpc, write_source_, now_ms);
}

void As11SettingsManager::complete_refresh(
    const RpcRequestCompletion &completion,
    uint32_t now_ms) {
    bool complete_snapshot = false;
    const bool applied = completion_succeeded(completion) &&
        state_.apply_settings_get_response(completion.payload, now_ms,
                                           &complete_snapshot);
    const bool succeeded = applied && complete_snapshot;
    note_change();

    if (!succeeded) {
        schedule_refresh(refresh_source_, now_ms);
        return;
    }

    refresh_retry_pending_ = false;
    next_refresh_retry_ms_ = 0;
    if (!refresh_again_pending_) return;

    refresh_again_pending_ = false;
    schedule_refresh(refresh_source_, now_ms, 0);
}

bool As11SettingsManager::background_source(RpcSource source) {
    return source == RpcSource::Scheduler || source == RpcSource::Internal;
}

bool As11SettingsManager::completion_succeeded(
    const RpcRequestCompletion &completion) {
    return completion.cause == RpcCompletionCause::Response &&
           completion.outcome.disposition ==
               OperationDisposition::Succeeded &&
           !completion.response_error;
}

}  // namespace aircannect
