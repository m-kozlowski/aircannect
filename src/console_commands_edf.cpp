#include "console_commands.h"

#include "config_service.h"
#include "edf_recorder_manager.h"
#include "management_console_utils.h"
#include "storage_service.h"

namespace aircannect {
namespace {

void print_edf_recorder_status(Print &out,
                               const EdfRecorderManager &manager) {
    const EdfRecorderStatus &status = manager.status();
    const EdfStreamAssemblerStatus &assembly = manager.assembler_status();
    const StorageEdfStatusSnapshot storage =
        StorageService::edf_status_snapshot();

    out.print("[EDF] enabled=");
    out.print(status.enabled ? "yes" : "no");
    out.print(" active=");
    out.print(status.active ? "yes" : "no");
    out.print(" stream=");
    out.print(status.stream_attached ? "attached" : "idle");
    out.print(" files=");
    out.print(status.files_open() ? "open" : "closed");
    out.print(" event_observer=");
    out.print(status.event_observer_registered ? "yes" : "no");
    out.print(" event_subscription=");
    out.print(status.event_attached ? "attached" : "idle");
    out.print(" event_coverage=");
    out.print(status.event_coverage_uncertain ? "uncertain" : "clean");
    out.print(" event_gen=");
    out.print(static_cast<unsigned long>(
        status.event_subscription_generation));
    out.print(" event_gaps=");
    out.print(static_cast<unsigned long>(
        status.event_coverage_session_gaps()));
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
    out.print(" segment_rollovers=");
    out.print(static_cast<unsigned long>(status.segment_rollovers));
    out.print(" frames=");
    out.print(static_cast<unsigned long>(status.frames));
    out.print(" drops=");
    out.print(static_cast<unsigned long>(status.frame_drops));
    out.print(" event_frames=");
    out.print(static_cast<unsigned long>(status.event_frames));
    out.print(" events=");
    out.print(static_cast<unsigned long>(status.event_records));
    out.print(" resp=");
    out.print(static_cast<unsigned long>(status.respiratory_events));
    out.print(" csr=");
    out.print(static_cast<unsigned long>(status.csr_events));
    out.print(" records=");
    out.print(static_cast<unsigned long>(status.brp_records));
    out.print('/');
    out.print(static_cast<unsigned long>(status.pld_records));
    out.print('/');
    out.print(static_cast<unsigned long>(status.sa2_records));
    out.print('/');
    out.print(static_cast<unsigned long>(status.tcv_records));
    out.print('/');
    out.print(static_cast<unsigned long>(status.eve_records));
    out.print('/');
    out.print(static_cast<unsigned long>(status.csl_records));
    out.print('/');
    out.print(static_cast<unsigned long>(status.str_records));
    out.print(" record_queue_failures=");
    out.print(static_cast<unsigned long>(status.record_enqueue_failures));
    out.print(" record_drops=");
    out.print(static_cast<unsigned long>(status.numeric_record_drops));
    out.print(" zle=");
    out.print(status.recording_gate_open()
                  ? "open"
                  : (status.recording_gate_closed() ? "closed" : "waiting"));
    out.print(" zle_edges=");
    out.print(static_cast<unsigned long>(status.recording_gate_rises));
    out.print('/');
    out.print(static_cast<unsigned long>(status.recording_gate_falls));
    if (status.recording_gate_recoveries) {
        out.print(" zle_recoveries=");
        out.print(static_cast<unsigned long>(
            status.recording_gate_recoveries));
    }
    if (status.recording_gate_recovery_pending()) {
        out.print(" zle_recovery=pending");
    }
    out.print(" zle_bad=");
    out.print(static_cast<unsigned long>(status.recording_gate_bad_events));
    out.print(" mask_event=");
    out.print(status.last_mask_event_time[0]
                  ? status.last_mask_event_time
                  : "--");
    if (status.annotation_open_pending()) {
        out.print(" annotation_pending=yes");
    }
    out.print(" mask_events=");
    out.print(static_cast<unsigned long>(status.mask_events));
    out.print('/');
    out.print(static_cast<unsigned long>(status.mask_bad_events));
    out.print(" numeric_open_buffered=");
    out.print(static_cast<unsigned long>(
        status.numeric_open_buffered_frames));
    out.print(" numeric_open_buffer_drops=");
    out.print(static_cast<unsigned long>(status.numeric_open_buffer_drops));
    out.print(" annotation_queue_failures=");
    out.print(static_cast<unsigned long>(
        status.annotation_enqueue_failures));
    out.print(" str_queue_failures=");
    out.print(static_cast<unsigned long>(status.str_enqueue_failures));
    out.print(" str_settings=");
    out.print(static_cast<unsigned long>(status.str_setting_requests));
    out.print('/');
    out.print(static_cast<unsigned long>(status.str_setting_responses));
    out.print(" values=");
    out.print(static_cast<unsigned long>(status.str_setting_values));
    out.print(" str_summary=");
    out.print(static_cast<unsigned long>(status.str_summary_requests));
    out.print('/');
    out.print(static_cast<unsigned long>(status.str_summary_responses));
    out.print(" values=");
    out.print(static_cast<unsigned long>(status.str_summary_values));
    out.print(" missing=");
    out.print(static_cast<unsigned long>(status.str_summary_missing));
    out.print(" unmapped=");
    out.print(static_cast<unsigned long>(status.str_summary_unmapped));
    out.print(" identification=");
    out.print(static_cast<unsigned long>(status.identification_requests));
    out.print('/');
    out.print(static_cast<unsigned long>(status.identification_responses));
    out.print(" writes=");
    out.print(static_cast<unsigned long>(
        status.identification_write_requests));
    out.print(" failures=");
    out.print(static_cast<unsigned long>(status.identification_failures));
    out.print(" file_open_failures=");
    out.print(static_cast<unsigned long>(status.file_open_failures));
    out.print(" attach_failures=");
    out.print(static_cast<unsigned long>(status.attach_failures));
    out.print(" storage_q=");
    out.print(static_cast<unsigned>(storage.queued));
    out.print('/');
    out.print(static_cast<unsigned>(storage.capacity));
    out.print(" storage_busy=");
    out.print(storage.busy ? "yes" : "no");
    out.print(" storage_open=");
    out.print(static_cast<unsigned>(storage.open_file_count));
    out.print(" storage_written=");
    out.print(static_cast<unsigned long>(storage.records_written));
    out.print(" storage_identification=");
    out.print(static_cast<unsigned long>(storage.identification_jobs));
    out.print(" storage_drops=");
    out.print(static_cast<unsigned long>(storage.queue_drops));
    out.print(" storage_patch_errors=");
    out.print(static_cast<unsigned long>(storage.patch_errors));
#if AC_STACK_PROFILE_ENABLED
    out.print(" storage_stack_free=");
    out.print(static_cast<unsigned long>(storage.stack_high_water_words));
#endif
    out.print(" assembly=");
    out.print(assembly.buffers_ready ? "ready" : "unavailable");
    out.print(" records=");
    out.print(static_cast<unsigned long>(assembly.records_completed));
    out.print(" samples=");
    out.print(static_cast<unsigned long>(assembly.samples_accepted));
    out.print(" invalid=");
    out.print(static_cast<unsigned long>(assembly.samples_invalid));
    out.print(" missing=");
    out.print(static_cast<unsigned long>(assembly.samples_missing));
    out.print(" dup=");
    out.print(static_cast<unsigned long>(assembly.samples_duplicate));
    out.print(" late=");
    out.print(static_cast<unsigned long>(assembly.samples_late));
    out.print(" ts_errors=");
    out.print(static_cast<unsigned long>(assembly.timestamp_errors));
    out.print(" ts_jitter_fix=");
    out.print(static_cast<unsigned long>(
        assembly.timestamp_jitter_corrections));
    out.print(" ts_resync=");
    out.print(static_cast<unsigned long>(assembly.timestamp_resyncs));
    out.print(" ts_jitter_ms=");
    out.print(static_cast<long>(assembly.last_timestamp_jitter_ms));
    out.print(" last_error=");
    if (status.last_error[0]) {
        out.print(status.last_error);
    } else if (assembly.last_error[0]) {
        out.print(assembly.last_error);
    } else if (storage.last_error[0]) {
        out.print(storage.last_error);
    } else {
        out.print("--");
    }
    if (status.last_event_name[0]) {
        out.print(" last_event=");
        out.print(status.last_event_name);
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

    print_unknown_command(out, "EDF", "edf, edf on, edf off");
    return true;
}

void EdfConsoleCommands::print_status(Print &out) {
    print_edf_recorder_status(out, recorder_);
}

void EdfConsoleCommands::print_stats(Print &out) {
    print_edf_recorder_status(out, recorder_);
}

}  // namespace aircannect
