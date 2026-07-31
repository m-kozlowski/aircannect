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
        storage.queue_drops;
    const bool has_faults = status.event_coverage_session_gaps() ||
        status.recording_gate_bad_events || status.mask_bad_events ||
        status.record_enqueue_failures || status.annotation_enqueue_failures ||
        status.str_enqueue_failures || status.file_open_failures ||
        status.attach_failures || storage.patch_errors ||
        assembly.timestamp_errors || last_error;

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

    out.print("[EDF capture] frames=");
    out.print(static_cast<unsigned long>(status.frames));
    out.print(" drops=");
    out.print(static_cast<unsigned long>(status.frame_drops));
    out.print(" segment_rollovers=");
    out.println(static_cast<unsigned long>(status.segment_rollovers));

    out.print("[EDF records] brp=");
    out.print(static_cast<unsigned long>(status.brp_records));
    out.print(" pld=");
    out.print(static_cast<unsigned long>(status.pld_records));
    out.print(" sa2=");
    out.print(static_cast<unsigned long>(status.sa2_records));
    out.print(" tcv=");
    out.print(static_cast<unsigned long>(status.tcv_records));
    out.print(" eve=");
    out.print(static_cast<unsigned long>(status.eve_records));
    out.print(" csl=");
    out.print(static_cast<unsigned long>(status.csl_records));
    out.print(" str=");
    out.println(static_cast<unsigned long>(status.str_records));

    out.print("[EDF events] frames=");
    out.print(static_cast<unsigned long>(status.event_frames));
    out.print(" records=");
    out.print(static_cast<unsigned long>(status.event_records));
    out.print(" respiratory=");
    out.print(static_cast<unsigned long>(status.respiratory_events));
    out.print(" csr=");
    out.print(static_cast<unsigned long>(status.csr_events));
    out.print(" generation=");
    out.print(static_cast<unsigned long>(
        status.event_subscription_generation));
    out.print(" gaps=");
    out.println(static_cast<unsigned long>(
        status.event_coverage_session_gaps()));

    out.print("[EDF gate] zle_edges=");
    out.print(static_cast<unsigned long>(status.recording_gate_rises));
    out.print('/');
    out.print(static_cast<unsigned long>(status.recording_gate_falls));
    out.print(" recoveries=");
    out.print(static_cast<unsigned long>(status.recording_gate_recoveries));
    out.print(" recovery_pending=");
    out.print(status.recording_gate_recovery_pending() ? "yes" : "no");
    out.print(" bad=");
    out.print(static_cast<unsigned long>(status.recording_gate_bad_events));
    out.print(" mask_events=");
    out.print(static_cast<unsigned long>(status.mask_events));
    out.print(" mask_bad=");
    out.print(static_cast<unsigned long>(status.mask_bad_events));
    out.print(" open_buffered=");
    out.print(static_cast<unsigned long>(status.numeric_open_buffered_frames));
    out.print(" open_buffer_drops=");
    out.println(static_cast<unsigned long>(status.numeric_open_buffer_drops));

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
    out.println(static_cast<unsigned long>(status.attach_failures));

    out.print("[EDF STR] settings=");
    out.print(static_cast<unsigned long>(status.str_setting_requests));
    out.print('/');
    out.print(static_cast<unsigned long>(status.str_setting_responses));
    out.print(" setting_values=");
    out.print(static_cast<unsigned long>(status.str_setting_values));
    out.print(" summary=");
    out.print(static_cast<unsigned long>(status.str_summary_requests));
    out.print('/');
    out.print(static_cast<unsigned long>(status.str_summary_responses));
    out.print(" summary_values=");
    out.print(static_cast<unsigned long>(status.str_summary_values));
    out.print(" missing=");
    out.print(static_cast<unsigned long>(status.str_summary_missing));
    out.print(" unmapped=");
    out.println(static_cast<unsigned long>(status.str_summary_unmapped));

    out.print("[EDF identification] requests=");
    out.print(static_cast<unsigned long>(status.identification_requests));
    out.print(" responses=");
    out.print(static_cast<unsigned long>(status.identification_responses));
    out.print(" writes=");
    out.print(static_cast<unsigned long>(
        status.identification_write_requests));
    out.print(" failures=");
    out.println(static_cast<unsigned long>(status.identification_failures));

    out.print("[EDF storage stats] records_written=");
    out.print(static_cast<unsigned long>(storage.records_written));
    out.print(" identification_jobs=");
    out.print(static_cast<unsigned long>(storage.identification_jobs));
    out.print(" queue_drops=");
    out.print(static_cast<unsigned long>(storage.queue_drops));
    out.print(" patch_errors=");
    out.println(static_cast<unsigned long>(storage.patch_errors));

    out.print("[EDF assembly] records=");
    out.print(static_cast<unsigned long>(assembly.records_completed));
    out.print(" samples=");
    out.print(static_cast<unsigned long>(assembly.samples_accepted));
    out.print(" invalid=");
    out.print(static_cast<unsigned long>(assembly.samples_invalid));
    out.print(" missing=");
    out.print(static_cast<unsigned long>(assembly.samples_missing));
    out.print(" duplicate=");
    out.print(static_cast<unsigned long>(assembly.samples_duplicate));
    out.print(" late=");
    out.print(static_cast<unsigned long>(assembly.samples_late));
    out.print(" timestamp_errors=");
    out.print(static_cast<unsigned long>(assembly.timestamp_errors));
    out.print(" jitter_corrections=");
    out.print(static_cast<unsigned long>(
        assembly.timestamp_jitter_corrections));
    out.print(" resyncs=");
    out.print(static_cast<unsigned long>(assembly.timestamp_resyncs));
    out.print(" last_jitter_ms=");
    out.println(static_cast<long>(assembly.last_timestamp_jitter_ms));

    if (status.last_event_name[0]) {
        out.print("[EDF last] event=");
        out.println(status.last_event_name);
    }
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

    print_unknown_command(out, "EDF", "edf, edf stats, edf on, edf off");
    return true;
}

void EdfConsoleCommands::print_status(Print &out) {
    print_edf_recorder_status(out, recorder_);
}

}  // namespace aircannect
