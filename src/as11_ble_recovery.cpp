#include "as11_ble_recovery.h"

#include <algorithm>

#include "spool_event_parser.h"
#include "utc_time.h"

#ifdef ARDUINO
#include "debug_log.h"
#endif

namespace aircannect {
namespace {

constexpr uint16_t AS11_ACTIVITY_POWER_UP = 17;
constexpr int64_t FETCH_QUERY_OVERLAP_MS = 1000;
constexpr uint32_t FETCH_SUBMIT_RETRY_MS = 250;
constexpr uint32_t FETCH_SUBMIT_DEADLINE_MS = 30000;

enum class RecoveryLogLevel : uint8_t {
    Warn,
    Info,
    Debug,
};

struct PowerUpSearch {
    int64_t after_ms = 0;
    int64_t power_up_ms = 0;
};

bool find_power_up(void *context, const SpoolEventRecord &record) {
    PowerUpSearch *search = static_cast<PowerUpSearch *>(context);
    if (!search || record.type != AS11_ACTIVITY_POWER_UP ||
        record.start_ms <= search->after_ms) {
        return true;
    }

    if (search->power_up_ms == 0 || record.start_ms < search->power_up_ms) {
        search->power_up_ms = record.start_ms;
    }
    return true;
}

bool due(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

void log_recovery(RecoveryLogLevel level,
                  const char *message,
                  int64_t event_ms = 0) {
#ifdef ARDUINO
    log_level_t log_level = LOG_DEBUG;
    if (level == RecoveryLogLevel::Warn) log_level = LOG_WARN;
    if (level == RecoveryLogLevel::Info) log_level = LOG_INFO;

    if (event_ms > 0) {
        Log::logf(CAT_BLE, log_level, "AS11 %s event_ms=%lld\n", message,
                  static_cast<long long>(event_ms));
    } else {
        Log::logf(CAT_BLE, log_level, "AS11 %s\n", message);
    }
#else
    (void)level;
    (void)message;
    (void)event_ms;
#endif
}

}  // namespace

bool As11BleRecovery::begin(ReportSpoolPort &spool) {
    if (spool_) return false;
    spool_ = &spool;
    return true;
}

void As11BleRecovery::observe_link(bool selected,
                                   bool authenticated,
                                   const As11DeviceState &device,
                                   uint32_t now_ms) {
    if (selected != selected_) select_transport(selected);
    if (!selected_) return;

    if (!link_state_known_) {
        link_state_known_ = true;
        authenticated_ = authenticated;
        ready_seen_ = authenticated;
        return;
    }
    if (authenticated == authenticated_) return;

    authenticated_ = authenticated;
    if (!authenticated) {
        if (ready_seen_) note_disconnect(device, now_ms);
        return;
    }

    if (!ready_seen_) {
        ready_seen_ = true;
        return;
    }
    if (disconnected_) note_reconnect(now_ms);
}

void As11BleRecovery::observe_event(const As11EventFrame &frame) {
    if (frame.data_id != AC_SYSTEM_ACTIVITY_SPOOL_TYPE) return;

    for (size_t i = 0; i < frame.event_count; ++i) {
        const As11EventRecord &event = frame.events[i];
        int64_t event_ms = 0;
        const bool time_valid =
            parse_utc_iso8601_ms(event.report_time.c_str(), event_ms);

        const bool recovery_active =
            selected_ && ready_seen_ &&
            (disconnected_ || classification_pending_ ||
             fetch_state_ != FetchState::Idle);
        if (recovery_active && event.name == "PowerUp" && time_valid &&
            (!disconnect_boundary_valid_ ||
             event_ms > disconnect_boundary_device_ms_)) {
            confirm_boot(event_ms);
        }

        if (time_valid && event_ms > latest_activity_device_ms_) {
            latest_activity_device_ms_ = event_ms;
        }
    }
}

void As11BleRecovery::poll(uint32_t now_ms) {
    if (!spool_) return;

    if (fetch_ticket_.valid()) consume_fetch_completion();
    if (fetch_state_ == FetchState::AwaitingSubmission) {
        submit_fetch(now_ms);
    }
}

As11BleRecoveryActions As11BleRecovery::take_actions() {
    const As11BleRecoveryActions actions = actions_;
    actions_ = {};
    return actions;
}

void As11BleRecovery::select_transport(bool selected) {
    cancel_fetch();

    selected_ = selected;
    link_state_known_ = false;
    authenticated_ = false;
    ready_seen_ = false;
    disconnected_ = false;
    classification_pending_ = false;
    disconnect_boundary_valid_ = false;
    disconnect_boundary_device_ms_ = 0;
    latest_activity_device_ms_ = 0;
    submit_due_ms_ = 0;
    submit_deadline_ms_ = 0;
    actions_ = {};
}

void As11BleRecovery::note_disconnect(const As11DeviceState &device,
                                      uint32_t now_ms) {
    disconnected_ = true;
    classification_pending_ = false;
    capture_disconnect_boundary(device, now_ms);
    cancel_fetch();
}

void As11BleRecovery::note_reconnect(uint32_t now_ms) {
    disconnected_ = false;
    classification_pending_ = disconnect_boundary_valid_;
    actions_.refresh = true;

    if (!classification_pending_) {
        log_recovery(RecoveryLogLevel::Warn,
                     "BLE reconnect boot check skipped: no device cursor");
        return;
    }

    submit_due_ms_ = now_ms;
    submit_deadline_ms_ = now_ms + FETCH_SUBMIT_DEADLINE_MS;
    if (fetch_state_ == FetchState::Idle) {
        fetch_state_ = FetchState::AwaitingSubmission;
    }
}

void As11BleRecovery::capture_disconnect_boundary(
    const As11DeviceState &device,
    uint32_t now_ms) {
    int64_t boundary_ms = latest_activity_device_ms_;
    int64_t sampled_device_ms = 0;
    if (device.clock_valid() &&
        parse_utc_iso8601_ms(device.device_datetime().c_str(),
                             sampled_device_ms)) {
        const uint32_t elapsed_ms = now_ms - device.clock_sample_ms();
        const int64_t estimated_device_ms =
            sampled_device_ms + static_cast<int64_t>(elapsed_ms);
        boundary_ms = std::max(boundary_ms, estimated_device_ms);
    }

    disconnect_boundary_valid_ = boundary_ms > 0;
    disconnect_boundary_device_ms_ = boundary_ms;
}

void As11BleRecovery::submit_fetch(uint32_t now_ms) {
    if (!classification_pending_) {
        fetch_state_ = FetchState::Idle;
        return;
    }
    if (!due(now_ms, submit_due_ms_)) return;
    if (due(now_ms, submit_deadline_ms_)) {
        finish_classification();
        log_recovery(RecoveryLogLevel::Warn,
                     "BLE reconnect boot check unavailable: spool busy");
        return;
    }

    ReportSpoolFetchCommand command;
    command.kind = ReportSpoolFetchKind::SystemActivity;
    command.from_ms = std::max<int64_t>(
        1, disconnect_boundary_device_ms_ - FETCH_QUERY_OVERLAP_MS);
    command.generation = next_generation();

    const OperationSubmission submission = spool_->request_fetch(command);
    if (submission.accepted()) {
        fetch_ticket_ = submission.ticket;
        fetch_state_ = FetchState::Fetching;
        return;
    }
    if (submission.admission == OperationAdmission::Busy) {
        submit_due_ms_ = now_ms + FETCH_SUBMIT_RETRY_MS;
        return;
    }

    finish_classification();
    log_recovery(RecoveryLogLevel::Warn,
                 "BLE reconnect boot check rejected by spool service");
}

void As11BleRecovery::consume_fetch_completion() {
    ReportSpoolFetchCompletion completion;
    if (!spool_->take_completion(fetch_ticket_, completion)) return;

    fetch_ticket_ = {};
    if (fetch_state_ == FetchState::Cancelling) {
        fetch_state_ = classification_pending_
            ? FetchState::AwaitingSubmission
            : FetchState::Idle;
        return;
    }

    fetch_state_ = FetchState::Idle;
    if (completion.outcome.disposition !=
        OperationDisposition::Succeeded) {
        finish_classification();
        log_recovery(RecoveryLogLevel::Warn,
                     "BLE reconnect boot check failed");
        return;
    }

    classify_result(completion.result);
}

void As11BleRecovery::classify_result(ReportSpoolResult &result) {
    char error[64] = {};
    if (!spool_result_valid_for_type(result,
                                     AC_SYSTEM_ACTIVITY_SPOOL_TYPE,
                                     error,
                                     sizeof(error))) {
        finish_classification();
        log_recovery(RecoveryLogLevel::Warn,
                     "BLE reconnect boot check returned invalid spool");
        return;
    }

    PowerUpSearch search;
    search.after_ms = disconnect_boundary_device_ms_;
    SpoolEventParseStats stats;
    if (!spool_parse_event_records(result.payload.data(),
                                   result.payload.size(),
                                   find_power_up,
                                   &search,
                                   &stats) ||
        stats.malformed_records != 0) {
        finish_classification();
        log_recovery(RecoveryLogLevel::Warn,
                     "BLE reconnect boot check could not parse activity");
        return;
    }

    if (search.power_up_ms > 0) {
        confirm_boot(search.power_up_ms);
        return;
    }

    finish_classification();
    log_recovery(RecoveryLogLevel::Debug,
                 "BLE reconnect had no AS11 PowerUp");
}

void As11BleRecovery::confirm_boot(int64_t event_ms) {
    authenticated_ = true;
    disconnected_ = false;
    actions_.refresh = false;
    actions_.boot_confirmed = true;
    finish_classification();
    log_recovery(RecoveryLogLevel::Info,
                 "boot confirmed after BLE reconnect", event_ms);
}

void As11BleRecovery::finish_classification() {
    classification_pending_ = false;
    if (fetch_ticket_.valid()) {
        cancel_fetch();
    } else {
        fetch_state_ = FetchState::Idle;
    }
}

void As11BleRecovery::cancel_fetch() {
    if (!fetch_ticket_.valid() || !spool_) {
        if (fetch_state_ != FetchState::Cancelling) {
            fetch_state_ = FetchState::Idle;
        }
        return;
    }

    (void)spool_->cancel(fetch_ticket_);
    fetch_state_ = FetchState::Cancelling;
}

uint32_t As11BleRecovery::next_generation() {
    generation_++;
    if (generation_ == 0) generation_ = 1;
    return generation_;
}

}  // namespace aircannect
