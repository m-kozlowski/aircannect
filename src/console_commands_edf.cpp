#include "console_commands.h"

#include "config_service.h"
#include "edf_recorder_manager.h"
#include "management_console_utils.h"
#include "storage_service.h"

namespace aircannect {
namespace {

const char *recording_gate_name(const EdfRecorderStatus &status) {
    if (status.recording_gate_open()) return "open";
    if (status.recording_gate_closed()) return "closed";
    return "waiting";
}

const char *last_edf_error(const EdfRecorderStatus &status,
                           const EdfStreamAssemblerStatus &assembly,
                           const StorageEdfStatusSnapshot &storage) {
    if (status.last_error[0]) return status.last_error;
    if (assembly.last_error[0]) return assembly.last_error;
    if (storage.last_error[0]) return storage.last_error;
    return nullptr;
}

void print_edf_recorder_status(Print &out,
                               const EdfRecorderManager &manager) {
    const EdfRecorderStatus &status = manager.status();
    const EdfStreamAssemblerStatus &assembly = manager.assembler_status();
    const StorageEdfStatusSnapshot storage =
        StorageService::edf_status_snapshot();
    const char *last_error = last_edf_error(status, assembly, storage);

    out.print("[EDF] enabled=");
    out.print(status.enabled ? "yes" : "no");
    out.print(" active=");
    out.print(status.active ? "yes" : "no");
    out.print(" stream=");
    out.print(status.stream_attached ? "attached" : "idle");
    out.print(" files=");
    out.print(status.files_open() ? "open" : "closed");
    out.print(" session=");
    out.print(static_cast<unsigned long>(status.session_id));
    out.print(" sessions=");
    out.print(static_cast<unsigned long>(status.sessions_started));
    out.print('/');
    out.print(static_cast<unsigned long>(status.sessions_ended));
    out.print(" clock=");
    out.print(status.clock_correction_applied ? "utc_corrected" : "raw");
    if (status.clock_correction_applied) {
        out.print(" clock_offset_ms=");
        out.print(static_cast<long long>(status.clock_correction_ms));
    }
    out.print(" sa2_input=");
    out.print(status.local_sa2_active ? "local" : "as11");
    out.println();

    out.print("[EDF input] events=");
    out.print(status.event_attached ? "attached" : "idle");
    out.print(" coverage=");
    out.print(status.event_coverage_uncertain ? "uncertain" : "clean");
    if (!status.event_observer_registered) out.print(" observer=missing");
    out.print(" zle=");
    out.print(recording_gate_name(status));
    out.print(" mask_event=");
    out.print(status.last_mask_event_time[0]
                  ? status.last_mask_event_time
                  : "--");
    if (status.annotation_open_pending()) out.print(" annotation=pending");
    out.println();

    out.print("[EDF storage] q=");
    out.print(static_cast<unsigned>(storage.queued));
    out.print('/');
    out.print(static_cast<unsigned>(storage.capacity));
    out.print(" busy=");
    out.print(storage.busy ? "yes" : "no");
    out.print(" open=");
    out.print(static_cast<unsigned>(storage.open_file_count));
    out.print(" assembly=");
    out.print(assembly.buffers_ready ? "ready" : "unavailable");
#if AC_STACK_PROFILE_ENABLED
    out.print(" stack_free=");
    out.print(static_cast<unsigned long>(storage.stack_high_water_words));
#endif
    out.println();

    const bool has_drops = status.frame_drops ||
        status.numeric_record_drops || status.numeric_open_buffer_drops ||
        status.local_sa2_queue_drops ||
        storage.queue_drops;
    const bool has_faults = status.event_coverage_session_gaps() ||
        status.recording_gate_bad_events || status.mask_bad_events ||
        status.record_enqueue_failures || status.annotation_enqueue_failures ||
        status.str_enqueue_failures || status.file_open_failures ||
        status.attach_failures || status.metadata_failures ||
        storage.patch_errors ||
        assembly.timestamp_errors || assembly.unmapped_signal[0] || last_error;

    if (has_drops || has_faults) {
        out.print("[EDF health] drops=");
        out.print(has_drops ? "present" : "none");
        out.print(" faults=");
        out.print(has_faults ? "present" : "none");
        if (last_error) {
            out.print(" last_error=");
            out.print(last_error);
        }
        out.println();
    }
}

void print_edf_recorder_stats(Print &out,
                              const EdfRecorderManager &manager) {
    const EdfRecorderStatus &status = manager.status();
    const EdfStreamAssemblerStatus &assembly = manager.assembler_status();
    const StorageEdfStatusSnapshot storage =
        StorageService::edf_status_snapshot();

    const auto series_total = [&assembly](
        uint32_t EdfSeriesAssemblyStatus::*field) {
        uint32_t total = 0;
        for (const EdfSeriesAssemblyStatus &series : assembly.series) {
            total += series.*field;
        }
        return total;
    };

    out.print("[EDF capture] frames=");
    out.print(static_cast<unsigned long>(assembly.frames));
    out.print(" drops=");
    out.print(static_cast<unsigned long>(status.frame_drops));
    out.print(" segment_rollovers=");
    out.println(static_cast<unsigned long>(status.segment_rollovers));

    out.print("[EDF records]");
    size_t schema_count = 0;
    const EdfFileSchema *schemas = edf_numeric_schemas(schema_count);
    for (size_t i = 0; i < schema_count; ++i) {
        out.print(' ');
        for (const char *tag = schemas[i].suffix; tag && *tag; ++tag) {
            out.print(*tag >= 'A' && *tag <= 'Z' ? *tag - 'A' + 'a' : *tag);
        }
        out.print('=');
        out.print(static_cast<unsigned long>(status.numeric_records[i]));
    }
    out.print(" eve=");
    out.print(static_cast<unsigned long>(status.eve_records));
    out.print(" csl=");
    out.print(static_cast<unsigned long>(status.csl_records));
    out.print(" str=");
    out.println(static_cast<unsigned long>(status.str_records));

    out.print("[EDF events] records=");
    out.print(static_cast<unsigned long>(status.event_records));
    out.print(" gaps=");
    out.println(static_cast<unsigned long>(
        status.event_coverage_session_gaps()));

    out.print("[EDF gate] recoveries=");
    out.print(static_cast<unsigned long>(status.recording_gate_recoveries));
    out.print(" recovery_pending=");
    out.print(status.recording_gate_recovery_pending() ? "yes" : "no");
    out.print(" bad=");
    out.print(static_cast<unsigned long>(status.recording_gate_bad_events));
    out.print(" mask_bad=");
    out.print(static_cast<unsigned long>(status.mask_bad_events));
    out.print(" open_buffer_drops=");
    out.print(static_cast<unsigned long>(status.numeric_open_buffer_drops));
    if (status.local_sa2_active || status.local_sa2_queue_drops) {
        out.print(" local_sa2_drops=");
        out.print(static_cast<unsigned long>(status.local_sa2_queue_drops));
    }
    out.println();

    out.print("[EDF queues] record_failures=");
    out.print(static_cast<unsigned long>(status.record_enqueue_failures));
    out.print(" record_drops=");
    out.print(static_cast<unsigned long>(status.numeric_record_drops));
    out.print(" annotation_failures=");
    out.print(static_cast<unsigned long>(status.annotation_enqueue_failures));
    out.print(" str_failures=");
    out.print(static_cast<unsigned long>(status.str_enqueue_failures));
    out.print(" file_open_failures=");
    out.print(static_cast<unsigned long>(status.file_open_failures));
    out.print(" attach_failures=");
    out.print(static_cast<unsigned long>(status.attach_failures));
    out.print(" metadata_failures=");
    out.println(static_cast<unsigned long>(status.metadata_failures));

    out.print("[EDF storage] queued=");
    out.print(static_cast<unsigned>(storage.queued));
    out.print('/');
    out.print(static_cast<unsigned>(storage.capacity));
    out.print(" open=");
    out.print(static_cast<unsigned>(storage.open_file_count));
    out.print(" queue_drops=");
    out.print(static_cast<unsigned long>(storage.queue_drops));
    out.print(" patch_errors=");
    out.println(static_cast<unsigned long>(storage.patch_errors));

    out.print("[EDF assembly] records=");
    out.print(static_cast<unsigned long>(series_total(
        &EdfSeriesAssemblyStatus::records_completed)));
    out.print(" missing_slots=");
    out.print(static_cast<unsigned long>(series_total(
        &EdfSeriesAssemblyStatus::missing_slots)));
    out.print(" duplicate=");
    out.print(static_cast<unsigned long>(series_total(
        &EdfSeriesAssemblyStatus::samples_duplicate)));
    out.print(" late=");
    out.print(static_cast<unsigned long>(series_total(
        &EdfSeriesAssemblyStatus::samples_late)));
    out.print(" timestamp_errors=");
    out.print(static_cast<unsigned long>(assembly.timestamp_errors));
    out.print(" resyncs=");
    out.print(static_cast<unsigned long>(assembly.timestamp_resyncs));
    if (assembly.unmapped_signal[0]) {
        out.print(" unmapped_signal=");
        out.print(assembly.unmapped_signal);
    }
    out.println();
}

bool parse_refresh_range(const String &rest,
                         SleepDayId &start_day,
                         SleepDayId &end_day) {
    int position = 0;
    String edf_object;
    String action;
    String start;
    String end;
    String extra;
    if (!parse_console_arg(rest, position, edf_object) ||
        !parse_console_arg(rest, position, action) ||
        !parse_console_arg(rest, position, start) ||
        edf_object != "str" || action != "refresh" ||
        start.length() != 8 ||
        !SleepDayId::from_yyyymmdd(start.c_str(), start_day)) {
        return false;
    }

    if (!parse_console_arg(rest, position, end)) {
        end_day = start_day;
        return true;
    }
    if (end.length() != 8 ||
        !SleepDayId::from_yyyymmdd(end.c_str(), end_day) ||
        end_day < start_day) {
        return false;
    }
    return !parse_console_arg(rest, position, extra);
}

void print_refresh_range(Print &out,
                         const char *prefix,
                         SleepDayId start_day,
                         SleepDayId end_day) {
    char start[9] = {};
    char end[9] = {};
    (void)start_day.format_yyyymmdd(start, sizeof(start));
    (void)end_day.format_yyyymmdd(end, sizeof(end));

    out.print(prefix);
    out.print(start);
    if (end_day != start_day) {
        out.print("..");
        out.print(end);
    }
    out.println();
}

}  // namespace

EdfConsoleCommands::EdfConsoleCommands(EdfRecorderManager &recorder,
                                       ConfigService &config)
    : recorder_(recorder), config_(config) {}

bool EdfConsoleCommands::execute(const String &command,
                                 const String &rest_arg,
                                 Print &out,
                                 ConsoleCommandSession &session) {
    (void)session;
    if (command != "edf") return false;

    String rest = rest_arg;
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        print_edf_recorder_status(out, recorder_);
        return true;
    }

    if (rest == "stats") {
        print_edf_recorder_stats(out, recorder_);
        return true;
    }

    if (rest.startsWith("str refresh")) {
        SleepDayId start_day;
        SleepDayId end_day;
        if (!parse_refresh_range(rest, start_day, end_day)) {
            out.println("[EDF] usage: edf str refresh YYYYMMDD [YYYYMMDD]");
            return true;
        }

        refresh_generation_++;
        if (refresh_generation_ == 0) refresh_generation_++;
        const OperationAdmission admission =
            recorder_.request_str_summary_refresh(
                start_day, end_day, refresh_generation_);
        if (admission == OperationAdmission::Busy) {
            out.println("[EDF] STR refresh busy");
            return true;
        }
        if (admission != OperationAdmission::Accepted) {
            out.println("[EDF] STR refresh rejected");
            return true;
        }

        refresh_session_id_ = session.id;
        refresh_wait_generation_ = refresh_generation_;
        print_refresh_range(out,
                            "[EDF] STR refresh started ",
                            start_day,
                            end_day);
        return true;
    }

    if (rest == "on" || rest == "enable" || rest == "off" ||
        rest == "disable") {
        const bool enabled = rest == "on" || rest == "enable";
        ConfigTransactionResult transaction;
        const ConfigFieldUpdate update = config_.set_value(
            "edf_cap", enabled ? "1" : "0", false, &transaction);
        if (!update.accepted() || !transaction.persisted) {
            out.println(enabled
                            ? "[EDF] warning: failed to persist enabled state"
                            : "[EDF] warning: failed to persist disabled state");
        }
        print_edf_recorder_status(out, recorder_);
        return true;
    }

    print_unknown_command(
        out,
        "EDF",
        "edf, edf stats, edf on, edf off, "
        "edf str refresh YYYYMMDD [YYYYMMDD]");
    return true;
}

void EdfConsoleCommands::poll_pending(Print &out,
                                      ConsoleCommandSession &session) {
    if (!refresh_session_id_ || session.id != refresh_session_id_) return;

    const EdfStrSummaryRefreshStatus &status =
        recorder_.str_summary_refresh_status();
    if (status.generation != refresh_wait_generation_) {
        out.println("[EDF] STR refresh failed error=result_superseded");
        refresh_session_id_ = 0;
        refresh_wait_generation_ = 0;
        return;
    }
    if (status.active()) return;

    if (status.state == EdfStrSummaryRefreshState::Complete) {
        out.print("[EDF] STR refresh complete matched=");
        out.print(static_cast<unsigned long>(status.matched));
        out.print(" updated=");
        out.print(static_cast<unsigned long>(status.updated));
        out.print(" unchanged=");
        out.print(static_cast<unsigned long>(status.unchanged));
        out.print(" missing=");
        out.println(static_cast<unsigned long>(status.missing));
    } else if (status.state == EdfStrSummaryRefreshState::Failed) {
        out.print("[EDF] STR refresh failed error=");
        out.println(status.error[0] ? status.error : "unknown");
    }

    refresh_session_id_ = 0;
    refresh_wait_generation_ = 0;
}

bool EdfConsoleCommands::pending_output(
    const ConsoleCommandSession &session) const {
    return refresh_session_id_ &&
        refresh_wait_generation_ &&
        session.id == refresh_session_id_;
}

void EdfConsoleCommands::cancel_pending(ConsoleCommandSession &session) {
    if (session.id != refresh_session_id_) return;

    refresh_session_id_ = 0;
    refresh_wait_generation_ = 0;
}

void EdfConsoleCommands::stop(ConsoleCommandSession &session) {
    cancel_pending(session);
}

void EdfConsoleCommands::print_status(Print &out) {
    print_edf_recorder_status(out, recorder_);
}

}  // namespace aircannect
