#include "management_console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "board.h"
#include "debug_log.h"
#include "export_coordinator.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "memory_manager.h"
#include "storage_diagnostic_job.h"
#include "storage_export_plan.h"
#include "storage_manager.h"
#include "string_util.h"
#include "tls_memory.h"
#include "version.h"
#include "web_ui.h"

namespace aircannect {
namespace {

bool json_number_literal(const String &value) {
    if (!value.length()) return false;
    const char first = value[0];
    if (first != '-' && !isdigit(static_cast<unsigned char>(first))) {
        return false;
    }
    bool digit = false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (isdigit(static_cast<unsigned char>(value[i]))) {
            digit = true;
            break;
        }
    }
    if (!digit) return false;

    char *end = nullptr;
    strtod(value.c_str(), &end);
    return end && end != value.c_str() && *end == '\0';
}

void print_web_buffer_memory(Print &out,
                             const char *name,
                             const WebUiBufferMemoryStatus &buffer,
                             size_t &total_capacity) {
    total_capacity += buffer.capacity;
    out.print("[MEM web] buffer=");
    out.print(name);
    out.print(" len=");
    out.print(static_cast<unsigned long>(buffer.length));
    out.print(" cap=");
    out.print(static_cast<unsigned long>(buffer.capacity));
    out.println();
}

void print_storage_test_status(Print &out,
                               const StorageDiagnosticStatus &status) {
    out.print("[STORAGE_TEST] state=");
    out.print(storage_diagnostic_state_name(status.state));
    out.print(" path=");
    out.print(status.path[0] ? status.path : "--");
    out.print(" bytes=");
    out.print(status.bytes);
    if (status.error[0]) {
        out.print(" error=");
        out.print(status.error);
    }
    out.println();
}

void print_uint64(Print &out, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    out.print(buf);
}

void print_web_memory_detail(Print &out, WebUI *web_ui) {
    if (!web_ui) {
        out.println("[MEM web] unavailable");
        return;
    }
    const WebUiMemoryStatus web = web_ui->memory_status();
    size_t total_capacity = 0;
    out.print("[MEM web] started=");
    out.print(web.started ? "yes" : "no");
    out.print(" sse_clients=");
    out.print(static_cast<unsigned long>(web.sse_clients));
    out.print(" sse_pending=");
    out.print(static_cast<unsigned long>(web.sse_pending_total));
    out.print(" sse_worst=");
    out.print(static_cast<unsigned long>(web.sse_pending_worst));
    out.print(" console_log_len=");
    out.print(static_cast<unsigned long>(web.console_log_length));
    out.println();
    print_web_buffer_memory(out, "status", web.status, total_capacity);
    print_web_buffer_memory(out, "stream", web.stream, total_capacity);
    print_web_buffer_memory(out, "console", web.console, total_capacity);
    print_web_buffer_memory(out, "config", web.config, total_capacity);
    print_web_buffer_memory(out, "wifi", web.wifi, total_capacity);
    print_web_buffer_memory(out, "oximetry_sensors",
                            web.oximetry_sensors, total_capacity);
    print_web_buffer_memory(out, "ota", web.ota, total_capacity);
    print_web_buffer_memory(out, "resmed_ota", web.resmed_ota,
                            total_capacity);
    print_web_buffer_memory(out, "settings", web.settings, total_capacity);
    print_web_buffer_memory(out, "live", web.live, total_capacity);
    out.print("[MEM web] buffer_cap_total=");
    out.print(static_cast<unsigned long>(total_capacity));
    out.println();
}

void print_owned_memory_detail(Print &out, ConsoleContext &ctx) {
    const StreamBroker &stream = ctx.stream;
    const size_t frame_pool_slots = stream.frame_pool_capacity();
    const size_t frame_pool_bytes =
        frame_pool_slots * sizeof(StreamFrameData) +
        frame_pool_slots * sizeof(StreamFrameData *);
    size_t stream_queue_total = 0;
    size_t stream_queue_capacity = 0;
    size_t stream_queue_worst = 0;
    uint32_t stream_queue_drops = 0;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        const StreamConsumerHandle handle =
            static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;
        const size_t queued = stream.consumer_queue_count(handle);
        stream_queue_total += queued;
        stream_queue_capacity += AC_STREAM_CONSUMER_QUEUE_DEPTH;
        if (queued > stream_queue_worst) stream_queue_worst = queued;
        stream_queue_drops += stream.consumer_queue_drops(handle);
    }
    const char *pending = "none";
    if (stream.pending_start()) pending = "start";
    else if (stream.pending_stop()) pending = "stop";
    out.print("[MEM stream] desired=");
    out.print(stream.desired_active() ? "yes" : "no");
    out.print(" actual=");
    out.print(stream.actual_active() ? "yes" : "no");
    out.print(" pending=");
    out.print(pending);
    out.print(" consumers=");
    out.print(static_cast<unsigned long>(stream.consumer_count()));
    out.print(" q=");
    out.print(static_cast<unsigned long>(stream_queue_total));
    out.print('/');
    out.print(static_cast<unsigned long>(stream_queue_capacity));
    out.print(" q_worst=");
    out.print(static_cast<unsigned long>(stream_queue_worst));
    out.print(" q_drops=");
    out.print(static_cast<unsigned long>(stream_queue_drops));
    out.println();
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        const StreamConsumerHandle handle =
            static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;
        out.print("[MEM stream consumer] id=");
        out.print(static_cast<unsigned long>(i));
        out.print(" source=");
        out.print(static_cast<unsigned long>(
            stream.consumer_source(handle)));
        out.print(" q=");
        out.print(static_cast<unsigned long>(
            stream.consumer_queue_count(handle)));
        out.print('/');
        out.print(static_cast<unsigned long>(
            AC_STREAM_CONSUMER_QUEUE_DEPTH));
        out.print(" drops=");
        out.print(static_cast<unsigned long>(
            stream.consumer_queue_drops(handle)));
        out.println();
    }
    out.print("[MEM owner] stream_frame_pool slots=");
    out.print(static_cast<unsigned long>(frame_pool_slots));
    out.print(" in_use=");
    out.print(static_cast<unsigned long>(stream.frame_pool_in_use()));
    out.print(" free=");
    out.print(static_cast<unsigned long>(stream.frame_pool_free()));
    out.print(" alloc_failures=");
    out.print(static_cast<unsigned long>(
        stream.frame_pool_allocation_failures()));
    out.print(" approx_bytes=");
    out.print(static_cast<unsigned long>(frame_pool_bytes));
    out.println();

    const OximetrySensorStatus oxi = ctx.oximetry_manager.sensor_status();
    out.print("[MEM owner] oximetry_sensor task=");
    out.print(oxi.sensor_task_started ? "started" : "stopped");
#if AC_STACK_PROFILE_ENABLED
    if (oxi.sensor_task_started) {
        out.print(" stack_free=");
        out.print(static_cast<unsigned long>(
            oxi.sensor_task_stack_high_water_bytes));
    }
#endif
    out.println();

    const TlsMemoryStatus tls = TlsMemory::status();
    out.print("[MEM owner] tls installed=");
    out.print(tls.installed ? "yes" : "no");
    out.print(" psram=");
    out.print(tls.psram_enabled ? "yes" : "no");
    out.print(" threshold=");
    out.print(static_cast<unsigned long>(tls.large_threshold));
    out.print(" large_psram=");
    out.print(static_cast<unsigned long>(tls.large_psram));
    out.print(" large_internal_fallback=");
    out.print(static_cast<unsigned long>(tls.large_internal_fallback));
    out.print(" large_internal_no_psram=");
    out.print(static_cast<unsigned long>(tls.large_internal_no_psram));
    out.print(" large_fail=");
    out.print(static_cast<unsigned long>(tls.large_fail));
    out.print(" small_internal=");
    out.print(static_cast<unsigned long>(tls.small_internal));
    out.print(" small_fail=");
    out.print(static_cast<unsigned long>(tls.small_fail));
    out.print(" frees=");
    out.print(static_cast<unsigned long>(tls.frees));
    out.println();
}

void print_edf_recorder_status(Print &out,
                               const EdfRecorderManager &manager) {
    const EdfRecorderStatus &status = manager.status();
    const EdfStreamAssemblerStatus &assembly = manager.assembler_status();
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
    out.print(status.recording_gate_open() ? "open" :
              (status.recording_gate_closed() ? "closed" : "waiting"));
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
    out.print(status.last_mask_event_time[0] ? status.last_mask_event_time : "--");
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
    out.print(static_cast<unsigned long>(
        status.numeric_open_buffer_drops));
    out.print(" annotation_queue_failures=");
    out.print(static_cast<unsigned long>(status.annotation_enqueue_failures));
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
    const StorageServiceStatus storage = manager.storage_status();
    out.print(" storage_q=");
    out.print(static_cast<unsigned>(storage.edf_queued));
    out.print('/');
    out.print(static_cast<unsigned>(storage.edf_capacity));
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

const char *report_task_state_name(ReportTaskState state) {
    switch (state) {
        case ReportTaskState::LoadingCatalog: return "loading_catalog";
        case ReportTaskState::IndexingArtifacts: return "indexing_artifacts";
        case ReportTaskState::Idle: return "idle";
        case ReportTaskState::RefreshingCatalog: return "refreshing_catalog";
        case ReportTaskState::Queued: return "queued";
        case ReportTaskState::LookingUp: return "looking_up";
        case ReportTaskState::Building: return "building";
        case ReportTaskState::Publishing: return "publishing";
        case ReportTaskState::Stopped:
        default: return "stopped";
    }
}

const char *report_engine_state_name(ReportEngineState state) {
    switch (state) {
        case ReportEngineState::Queued: return "queued";
        case ReportEngineState::WaitingForCatalog: return "waiting_catalog";
        case ReportEngineState::LookingUp: return "looking_up";
        case ReportEngineState::AcquiringFallback: return "fallback";
        case ReportEngineState::Executing: return "executing";
        case ReportEngineState::Publishing: return "publishing";
        case ReportEngineState::Idle:
        default: return "idle";
    }
}

const char *report_catalog_state_name(NightCatalogRefreshState state) {
    switch (state) {
        case NightCatalogRefreshState::Scanning: return "scanning";
        case NightCatalogRefreshState::ReadingEdf: return "reading_edf";
        case NightCatalogRefreshState::ReadingFallback:
            return "reading_fallback";
        case NightCatalogRefreshState::ReadingStr: return "reading_str";
        case NightCatalogRefreshState::Building: return "building";
        case NightCatalogRefreshState::Ready: return "ready";
        case NightCatalogRefreshState::Error: return "error";
        case NightCatalogRefreshState::Idle:
        default: return "idle";
    }
}

void print_report_status(Print &out, const ReportTask &task) {
    const ReportTaskStatus status = task.status();

    out.print("[REPORT] state=");
    out.print(report_task_state_name(status.state));
    out.print(" started=");
    out.print(status.task_started ? "yes" : "no");
    out.print(" nights=");
    out.print(static_cast<unsigned long>(status.catalog_nights));
    out.print(" generation=");
    out.print(static_cast<unsigned long>(status.catalog_generation));
    out.print(" commands=");
    out.print(static_cast<unsigned long>(status.commands_queued));
    out.print(" dropped=");
    out.print(static_cast<unsigned long>(status.command_drops));
    out.print(" failed=");
    out.print(static_cast<unsigned long>(status.command_failures));
    out.println();

    out.print("[REPORT] engine=");
    out.print(report_engine_state_name(status.engine.state));
    out.print(" queued=");
    out.print(static_cast<unsigned long>(status.engine.queued));
    out.print(" foreground=");
    out.print(status.foreground_active ? "yes" : "no");
    out.print(" background=");
    out.print(status.background_active ? "yes" : "no");
    out.print(" suspended=");
    out.print(status.background_suspended ? "yes" : "no");
    out.print(" last_error=");
    out.print(status.engine.last_completion.error[0]
                  ? status.engine.last_completion.error
                  : "--");
    out.println();

    out.print("[REPORT] catalog=");
    out.print(report_catalog_state_name(status.catalog_refresh.state));
    out.print(" files=");
    out.print(static_cast<unsigned long>(status.catalog_refresh.files_indexed));
    out.print("/");
    out.print(static_cast<unsigned long>(status.catalog_refresh.files_seen));
    out.print(" sessions=");
    out.print(static_cast<unsigned long>(status.catalog_refresh.sessions));
    out.print(" error=");
    out.print(status.catalog_refresh.error[0]
                  ? status.catalog_refresh.error
                  : "--");
    out.println();
}

uint32_t report_night_duration(const NightCatalog &catalog,
                               const NightCatalogRecord &night) {
    if (night.metrics.has(NightCatalogMetric::DurationMinutes)) {
        return night.metrics.duration_min;
    }

    uint64_t duration_ms = 0;
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, session_count);
    for (size_t i = 0; sessions && i < session_count; ++i) {
        if (!sessions[i].valid()) continue;
        duration_ms += static_cast<uint64_t>(
            sessions[i].end_ms - sessions[i].start_ms);
    }
    return static_cast<uint32_t>(duration_ms / 60000);
}

void print_report_nights(Print &out, const ReportTask &task) {
    const std::shared_ptr<const NightCatalog> catalog =
        task.catalog_snapshot();
    out.println("[REPORT nights]");
    if (!catalog || catalog->size() == 0) {
        out.println("  no therapy nights indexed");
        return;
    }

    for (size_t i = 0; i < catalog->size(); ++i) {
        const NightCatalogRecord *night = catalog->record(i);
        if (!night) continue;

        char day[9] = {};
        night->sleep_day.format_yyyymmdd(day, sizeof(day));
        out.print("  ");
        out.print(day);
        out.print(" duration_min=");
        out.print(static_cast<unsigned long>(
            report_night_duration(*catalog, *night)));
        out.print(" sessions=");
        out.print(static_cast<unsigned long>(night->session_count));
        out.print(" sources=0x");
        out.print(static_cast<unsigned>(night->source_flags), HEX);
        out.println();
    }
}

bool parse_report_sleep_day(String value, SleepDayId &sleep_day) {
    trim_inplace(value);
    return value.length() == 8 &&
           SleepDayId::from_yyyymmdd(value.c_str(), sleep_day);
}

std::string cli_set_value_literal(String value) {
    trim_inplace(value);
    String lower = value;
    to_lower_inplace(lower);
    if (lower == "true" || lower == "false" || lower == "null" ||
        json_number_literal(value) || value.startsWith("{") ||
        value.startsWith("[")) {
        return to_std(value);
    }

    std::string out = "\"";
    out += json_escape(to_std(value));
    out += "\"";
    return out;
}

void append_cli_set_pair(std::string &out,
                         bool &first,
                         const String &key,
                         const String &value) {
    if (!first) out += ",";
    out += "\"";
    out += json_escape(to_std(key));
    out += "\":";
    out += cli_set_value_literal(value);
    first = false;
}

void append_json_object_members(std::string &out,
                                bool &first,
                                const std::string &object) {
    if (object.size() < 2 || object.front() != '{' ||
        object.back() != '}') {
        return;
    }
    const size_t len = object.size() - 2;
    if (len == 0) return;
    if (!first) out += ",";
    out.append(object, 1, len);
    first = false;
}

}  // namespace

void ManagementConsole::execute_line(String line,
                                     Print &out,
                                     ConsoleContext &ctx) {
    trim_inplace(line);
    if (!line.length()) return;

    int pos = 0;
    String command;
    if (!parse_console_arg(line, pos, command)) return;
    to_lower_inplace(command);
    String rest = pos < static_cast<int>(line.length()) ? line.substring(pos)
                                                        : "";

    using Handler = void (ManagementConsole::*)(Print &, String,
                                                ConsoleContext &);
    struct CommandDef {
        const char *name;
        Handler handler;
    };
    static const CommandDef commands[] = {
        {"help", &ManagementConsole::handle_help_command},
        {"?", &ManagementConsole::handle_help_command},
        {"status", &ManagementConsole::handle_status_command},
        {"stats", &ManagementConsole::handle_stats_command},
        {"memory", &ManagementConsole::handle_memory_command},
        {"mem", &ManagementConsole::handle_memory_command},
        {"session", &ManagementConsole::handle_session_command},
        {"sink", &ManagementConsole::handle_sink_command},
        {"edf", &ManagementConsole::handle_edf_command},
        {"oxi", &ManagementConsole::handle_oximetry_command},
        {"oximetry", &ManagementConsole::handle_oximetry_command},
        {"report", &ManagementConsole::handle_report_command},
        {"storage", &ManagementConsole::handle_storage_command},
        {"smb", &ManagementConsole::handle_smb_command},
        {"sleephq", &ManagementConsole::handle_sleephq_command},
        {"as11", &ManagementConsole::handle_as11_command},
        {"therapy", &ManagementConsole::handle_therapy_command},
        {"config", &ManagementConsole::handle_config_command},
        {"wifi", &ManagementConsole::handle_wifi_command},
        {"tcp", &ManagementConsole::handle_tcp_command},
        {"ota", &ManagementConsole::handle_ota_command},
        {"resmed-ota", &ManagementConsole::handle_resmed_ota_command},
        {"log", &ManagementConsole::handle_log_command},
        {"restart", &ManagementConsole::handle_restart_command},
        {"can", &ManagementConsole::handle_can_command},
        {"version", &ManagementConsole::handle_version_command},
        {"v", &ManagementConsole::handle_version_command},
        {"time", &ManagementConsole::handle_time_command},
        {"get", &ManagementConsole::handle_get_command},
        {"set", &ManagementConsole::handle_set_command},
        {"stream", &ManagementConsole::handle_stream_command},
        {"rpc", &ManagementConsole::handle_rpc_command},
        {"raw", &ManagementConsole::handle_raw_command},
    };

    for (const CommandDef &entry : commands) {
        if (command == entry.name) {
            (this->*entry.handler)(out, rest, ctx);
            return;
        }
    }

    out.println("[CLI] unknown command. Type 'help'.");
}

void ManagementConsole::handle_help_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    (void)ctx;
    trim_inplace(rest);
    print_help(out, rest);
}

void ManagementConsole::handle_status_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    trim_inplace(rest);
    if (rest.length()) {
        print_unknown_command(out, "STATUS", "status");
        return;
    }
    ConsoleFormat::print_rpc_status(out, ctx.rpc_diagnostics, ctx.can);
    ConsoleFormat::print_as11_status(out, ctx.device.state());
    ConsoleFormat::print_session_status(out, ctx.session_manager.status());
    ConsoleFormat::print_sink_status(out, ctx.sink_manager);
    print_edf_recorder_status(out, ctx.edf_recorder_manager);
    print_oximetry_status(out, ctx.oximetry_manager);
}

void ManagementConsole::handle_stats_command(Print &out,
                                             String rest,
                                             ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (rest == "reset") {
        ctx.rpc_diagnostics.reset_stats();
        ctx.events.reset_counters();
        ctx.stream.reset_counters();
        out.println("[STATS] reset");
        return;
    }
    if (rest.length() && rest != "status") {
        print_unknown_command(out, "STATS", "stats, stats reset");
        return;
    }
    ConsoleFormat::print_rpc_stats(out, ctx.rpc_diagnostics, ctx.can,
                                   ctx.events, ctx.stream);
    ConsoleFormat::print_tcp_stats(out, ctx.tcp_bridge);
    ConsoleFormat::print_log_stats(out);
    ConsoleFormat::print_memory_status(out, Memory::status());
    ConsoleFormat::print_storage_status(out, Storage::status());
    ConsoleFormat::print_session_status(out, ctx.session_manager.status());
    ConsoleFormat::print_sink_status(out, ctx.sink_manager);
    print_edf_recorder_status(out, ctx.edf_recorder_manager);
    print_oximetry_status(out, ctx.oximetry_manager);
}

void ManagementConsole::handle_memory_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_memory_status(out, Memory::status());
        return;
    }
    if (rest == "detail") {
        ConsoleFormat::print_memory_detail_status(out,
                                                  Memory::detail_status());
        print_owned_memory_detail(out, ctx);
        print_web_memory_detail(out, ctx.web_ui);
        return;
    }
    print_unknown_command(out, "MEM", "memory, memory detail");
}

void ManagementConsole::handle_session_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (rest.length() && rest != "status") {
        print_unknown_command(out, "SESSION", "session status");
        return;
    }
    ConsoleFormat::print_session_status(out, ctx.session_manager.status());
}

void ManagementConsole::handle_sink_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    handle_sink(out, rest, ctx.sink_manager);
}

void ManagementConsole::handle_edf_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (!rest.length() || rest == "status") {
        print_edf_recorder_status(out, ctx.edf_recorder_manager);
        return;
    }
    if (rest == "on" || rest == "enable") {
        if (!ctx.app_config.set_edf_capture_enabled(true)) {
            out.println("[EDF] warning: failed to persist enabled state");
        }
        ctx.edf_recorder_manager.set_enabled(true);
        print_edf_recorder_status(out, ctx.edf_recorder_manager);
        return;
    }
    if (rest == "off" || rest == "disable") {
        if (!ctx.app_config.set_edf_capture_enabled(false)) {
            out.println("[EDF] warning: failed to persist disabled state");
        }
        ctx.edf_recorder_manager.set_enabled(false);
        print_edf_recorder_status(out, ctx.edf_recorder_manager);
        return;
    }
    print_unknown_command(out, "EDF", "edf, edf on, edf off");
}

void ManagementConsole::handle_oximetry_command(Print &out,
                                                String rest,
                                                ConsoleContext &ctx) {
    handle_oximetry(out, rest, ctx.oximetry_manager);
}

void ManagementConsole::handle_report_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (!rest.length() || rest == "status") {
        print_report_status(out, ctx.report_task);
        return;
    }
    if (rest == "nights" || rest == "list") {
        print_report_nights(out, ctx.report_task);
        return;
    }
    if (rest == "result") {
        out.println("[REPORT] usage: report result latest|YYYYMMDD");
        return;
    }
    if (rest.startsWith("result ")) {
        String value = rest.substring(strlen("result "));
        trim_inplace(value);

        const std::shared_ptr<const NightCatalog> catalog =
            ctx.report_task.catalog_snapshot();
        if (!catalog) {
            out.println("[REPORT] night catalog unavailable");
            return;
        }

        SleepDayId sleep_day;
        if (value == "latest") {
            const NightCatalogRecord *latest = catalog->record(0);
            if (!latest) {
                out.println("[REPORT] no indexed nights");
                return;
            }
            sleep_day = latest->sleep_day;
        } else if (!parse_report_sleep_day(value, sleep_day)) {
            out.println("[REPORT] usage: report result latest|YYYYMMDD");
            return;
        }

        const NightCatalogRecord *night = catalog->find(sleep_day);
        if (!night) {
            out.println("[REPORT] night not found");
            return;
        }

        static uint32_t generation = 0;
        generation++;
        if (generation == 0) generation = 1;

        const OperationAdmission admitted = ctx.report_task.request_artifact(
            ReportArtifactKey::result(sleep_day, night->source_revision),
            ReportRequestPriority::Foreground,
            generation);
        if (admitted == OperationAdmission::Accepted) {
            out.println("[REPORT] result requested");
        } else if (admitted == OperationAdmission::Busy) {
            out.println("[REPORT] request queue busy");
        } else {
            out.println("[REPORT] request rejected");
        }
        return;
    }
    print_unknown_command(
        out, "REPORT",
        "report, report status, report nights, report result latest|YYYYMMDD");
}

void ManagementConsole::handle_storage_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    trim_inplace(rest);
    String rest_lower = rest;
    to_lower_inplace(rest_lower);
    if (!rest_lower.length() || rest_lower == "status") {
        ConsoleFormat::print_storage_status(out, Storage::status());
        return;
    }
    if (rest_lower == "remount" || rest_lower == "retry") {
        Storage::remount();
        ConsoleFormat::print_storage_status(out, Storage::status());
        return;
    }
    if (rest_lower == "write-test status") {
        if (!ctx.storage_diagnostic_job) {
            out.println("[STORAGE_TEST] unavailable");
            return;
        }
        print_storage_test_status(out,
                                  ctx.storage_diagnostic_job->status());
        return;
    }
    if (rest_lower == "write-test" || rest_lower.startsWith("write-test ")) {
        String args = rest;
        args.remove(0, String("write-test").length());
        int pos = 0;
        String path = "/aircannect-write-test.txt";
        String text = "AirCANnect storage write test";
        String parsed;
        if (parse_console_arg(args, pos, parsed)) {
            path = parsed;
            if (parse_console_arg(args, pos, parsed)) text = parsed;
        }
        text += '\n';
        StorageDiagnosticJob *job = ctx.storage_diagnostic_job;
        const bool queued = job && job->request_append(
            path.c_str(),
            reinterpret_cast<const uint8_t *>(text.c_str()),
            text.length());

        out.print("[STORAGE_TEST] ");
        out.println(queued ? "queued" : "rejected");
        if (job) {
            print_storage_test_status(out, job->status());
        }
        return;
    }
    print_unknown_command(out, "STORAGE",
                          "storage status, remount, write-test [status|P T]");
}

void ManagementConsole::handle_smb_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    ExportCoordinator *coordinator = ctx.export_coordinator;
    if (!coordinator) {
        out.println("[SMB] unavailable");
        return;
    }
    if (!rest.length() || rest == "status") {
        const StorageSyncStatus status = coordinator->smb_status();
        out.print("[SMB] state=");
        out.print(storage_sync_state_name(status.state));
        out.print(" configured=");
        out.print(status.configured ? "yes" : "no");
        out.print(" network=");
        out.print(status.network_available ? "yes" : "no");
        out.print(" pending=");
        out.print(status.pending ? "yes" : "no");
        out.print(" reason=");
        out.print(status.pending_reason[0] ? status.pending_reason : "--");
        out.print(" endpoint=");
        out.print(status.endpoint_set ? "set" : "--");
        out.print(" user=");
        out.print(status.user_set ? "set" : "--");
        out.print(" last_verify=");
        print_uint64(out, status.last_verify_epoch);
        out.print(" last_sync=");
        print_uint64(out, status.last_sync_epoch);
        out.print(" last_failure=");
        print_uint64(out, status.last_failure_epoch);
        out.print(" error=");
        if (status.last_error[0]) {
            out.print(status.last_error);
        } else if (status.last_failure_error[0]) {
            out.print(status.last_failure_error);
        } else {
            out.print("--");
        }
        out.print(" files=");
        out.print(static_cast<unsigned>(status.files_uploaded));
        out.print("/");
        out.print(static_cast<unsigned>(status.files_seen));
        out.print(" skipped=");
        out.print(static_cast<unsigned>(status.files_skipped));
        out.print(" failed=");
        out.print(static_cast<unsigned>(status.files_failed));
        out.print(" bytes=");
        print_uint64(out, status.bytes_uploaded);
        out.print(" current=");
        out.print(status.current_path[0] ? status.current_path : "--");
        out.println();
        return;
    }
    if (rest == "verify" || rest == "check") {
        const bool queued = coordinator->request_smb_verify();
        out.print("[SMB] verify ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    if (rest == "sync") {
        const bool queued = coordinator->request_smb_sync();
        out.print("[SMB] sync ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    print_unknown_command(out, "SMB", "smb status, smb verify, smb sync");
}

void ManagementConsole::handle_sleephq_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    ExportCoordinator *coordinator = ctx.export_coordinator;
    if (!coordinator) {
        out.println("[SLEEPHQ] unavailable");
        return;
    }
    if (!rest.length() || rest == "status") {
        const SleepHqSyncStatus status = coordinator->sleephq_status();
        out.print("[SLEEPHQ] state=");
        out.print(sleephq_sync_state_name(status.state));
        out.print(" configured=");
        out.print(status.configured ? "yes" : "no");
        out.print(" network=");
        out.print(status.network_available ? "yes" : "no");
        out.print(" pending=");
        out.print(status.pending ? "yes" : "no");
        out.print(" reason=");
        out.print(status.pending_reason[0] ? status.pending_reason : "--");
        out.print(" team_id=");
        if (status.team_id) {
            out.print(static_cast<unsigned long>(status.team_id));
        } else {
            out.print("--");
        }
        out.print(" last_check=");
        print_uint64(out, status.last_check_epoch);
        out.print(" last_sync=");
        print_uint64(out, status.last_sync_epoch);
        out.print(" last_failure=");
        print_uint64(out, status.last_failure_epoch);
        out.print(" error=");
        out.print(status.last_error[0] ? status.last_error : "--");
        out.print(" import=");
        if (status.import_id) {
            out.print(static_cast<unsigned long>(status.import_id));
        } else {
            out.print("--");
        }
        out.print(" files=");
        out.print(static_cast<unsigned>(status.files_uploaded));
        out.print("/");
        out.print(static_cast<unsigned>(status.files_seen));
        out.print(" skipped=");
        out.print(static_cast<unsigned>(status.files_skipped));
        out.print(" bytes=");
        print_uint64(out, status.bytes_uploaded);
        out.print(" current=");
        out.print(status.current_path[0] ? status.current_path : "--");
        out.println();
        return;
    }
    if (rest == "check" || rest == "verify") {
        const bool queued = coordinator->request_sleephq_check();
        out.print("[SLEEPHQ] check ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    if (rest == "sync") {
        const bool queued = coordinator->request_sleephq_sync();
        out.print("[SLEEPHQ] sync ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    if (rest.startsWith("sync ")) {
        String day = rest.substring(5);
        trim_inplace(day);
        if (!storage_export_is_datalog_day_name(day.c_str())) {
            out.println("[SLEEPHQ] bad day; use YYYYMMDD");
            return;
        }
        const bool queued = coordinator->request_sleephq_sync_day(day.c_str());
        out.print("[SLEEPHQ] sync day=");
        out.print(day);
        out.print(" ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    print_unknown_command(out, "SLEEPHQ",
                          "sleephq status, sleephq check, sleephq sync, "
                          "sleephq sync YYYYMMDD");
}

void ManagementConsole::handle_as11_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    handle_as11(out, rest, ctx.rpc, ctx.rpc_passthrough, ctx.device);
}

void ManagementConsole::handle_therapy_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    handle_therapy(out, rest, ctx.rpc, ctx.device);
}

void ManagementConsole::handle_config_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    handle_config(out, rest, ctx.app_config, ctx.wifi_manager,
                  ctx.tcp_bridge, ctx.ota_manager,
                  ctx.edf_recorder_manager);
}

void ManagementConsole::handle_wifi_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    handle_wifi(out, rest, ctx.wifi_manager, ctx.tcp_bridge, ctx.app_config);
}

void ManagementConsole::handle_tcp_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (rest.length() && rest != "status") {
        print_unknown_command(out, "TCP", "tcp status");
        return;
    }
    ConsoleFormat::print_tcp_status(out, ctx.tcp_bridge);
}

void ManagementConsole::handle_ota_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    handle_ota(out, rest, ctx.ota_manager, ctx.resmed_ota_manager);
}

void ManagementConsole::handle_resmed_ota_command(Print &out,
                                                  String rest,
                                                  ConsoleContext &ctx) {
    handle_resmed_ota(out, rest, ctx.resmed_ota_manager);
}

void ManagementConsole::handle_log_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    handle_log(out, rest, ctx.app_config, ctx.storage_read_port);
}

void ManagementConsole::handle_restart_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    trim_inplace(rest);
    if (rest.length()) {
        print_unknown_command(out, "SYSTEM", "restart");
        return;
    }
    ctx.ota_manager.schedule_reboot(500);
    out.println("[SYSTEM] restart scheduled");
}

void ManagementConsole::handle_can_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_rpc_status(out, ctx.rpc_diagnostics, ctx.can);
        return;
    }
    if (rest == "restart") {
        ctx.rpc_diagnostics.recover_can("console CAN restart command");
        return;
    }
    print_unknown_command(out, "CAN", "can status, can restart");
}

void ManagementConsole::handle_version_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    (void)ctx;
    trim_inplace(rest);
    if (rest.length()) {
        print_unknown_command(out, "FW", "version");
        return;
    }
    out.print("[FW] AirCANnect ");
    out.print(aircannect_version());
    out.print(" built ");
    out.println(aircannect_build_date());
}

void ManagementConsole::handle_time_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    handle_time(out, rest, ctx.device, ctx.time_sync_service);
}

void ManagementConsole::handle_get_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    if (!rest.length()) {
        out.println("[RPC] usage: get NAME [NAME...]");
        return;
    }
    ctx.rpc_passthrough.send_request("Get", build_get_params(to_std(rest)),
                                     RpcSource::Console);
}

void ManagementConsole::handle_set_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    if (!rest.length()) {
        out.println(
            "[RPC] usage: set NAME VALUE [NAME VALUE...] | set {JSON_PARAMS}");
        return;
    }

    std::string params;
    bool managed_settings = false;
    if (rest.startsWith("{")) {
        params = to_std(rest);
    } else {
        int pos = 0;
        String key;
        String value;
        std::string raw_params = "{";
        std::string setting_body = "{";
        bool raw_first = true;
        bool setting_first = true;
        size_t raw_count = 0;
        size_t setting_count = 0;

        while (parse_console_arg(rest, pos, key)) {
            if (!parse_console_arg(rest, pos, value)) {
                out.println(
                    "[RPC] usage: set NAME VALUE [NAME VALUE...] | "
                    "set {JSON_PARAMS}");
                return;
            }

            if (key.startsWith("_")) {
                append_cli_set_pair(raw_params, raw_first, key, value);
                raw_count++;
            } else {
                append_cli_set_pair(setting_body, setting_first, key, value);
                setting_count++;
            }
        }
        raw_params += "}";
        setting_body += "}";

        const As11SettingsState &settings = ctx.settings_manager.state();
        const As11DeviceState &as11 = ctx.device.state();
        int mode = settings.mode_index();
        if (mode < 0) {
            mode = as11_mode_index_from_value(as11.active_therapy_profile());
        }

        size_t accepted = 0;
        std::string mapped_params = "{}";
        if (setting_count) {
            mapped_params =
                as11_build_set_params_from_json(setting_body, mode, accepted);
        }

        if (!raw_count && !accepted) {
            out.println("[RPC] no accepted settings");
            return;
        }
        managed_settings = accepted != 0;

        bool first = true;
        params = "{";
        append_json_object_members(params, first, raw_params);
        append_json_object_members(params, first, mapped_params);
        params += "}";
    }

    const bool queued = managed_settings
        ? ctx.settings_manager.write(ctx.rpc, params, RpcSource::Console,
                                     millis()).accepted()
        : ctx.rpc_passthrough.send_request("Set", params,
                                           RpcSource::Console);
    if (queued) {
        out.println("[RPC] Set queued");
    } else {
        out.println("[RPC] Set queue failed");
    }
}

void ManagementConsole::handle_stream_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    handle_stream(out, rest, ctx.stream);
}

void ManagementConsole::handle_rpc_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    int split = rest.indexOf(' ');
    String method = split < 0 ? rest : rest.substring(0, split);
    String params = split < 0 ? "" : rest.substring(split + 1);
    trim_inplace(params);
    if (!method.length()) {
        out.println("[RPC] usage: rpc METHOD [JSON_PARAMS]");
        return;
    }
    ctx.rpc_passthrough.send_request(to_std(method), to_std(params),
                                     RpcSource::Console);
}

void ManagementConsole::handle_raw_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    if (!rest.length()) {
        out.println("[RPC] usage: raw JSON");
        return;
    }
    ctx.rpc_passthrough.submit_raw_payload(to_std(rest), RpcSource::Console);
}

}  // namespace aircannect
