#include "management_console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "background_worker.h"
#include "board.h"
#include "board_report.h"
#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "export_coordinator.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "memory_manager.h"
#include "report_store.h"
#include "storage_manager.h"
#include "storage_writer.h"
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

bool report_store_integrity_allowed(const ConsoleContext &ctx,
                                    const char *&reason) {
    const SessionStatus &session = ctx.session_manager.status();
    if (session.state == SessionState::Active) {
        reason = "therapy_active";
        return false;
    }
    if (ctx.arbiter.stream_activity_active()) {
        reason = "stream_active";
        return false;
    }
    reason = "";
    return true;
}

void print_owned_memory_detail(Print &out, ConsoleContext &ctx) {
    const StreamBroker &stream = ctx.arbiter.stream_broker();
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

    const StorageWriterStatus storage = StorageWriter::status();
    out.print("[MEM owner] storage_writer psram=");
    out.print(storage.using_psram ? "yes" : "no");
    out.print(" q=");
    out.print(static_cast<unsigned long>(storage.queued));
    out.print('/');
    out.print(static_cast<unsigned long>(storage.capacity));
    out.print(" chunk=");
    out.print(static_cast<unsigned long>(storage.chunk_bytes));
    out.print(" data_bytes=");
    out.print(static_cast<unsigned long>(
        storage.capacity * storage.chunk_bytes));
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

void print_report_store_status(Print &out) {
    const ReportStoreStatus status = ReportStore::status();
    out.print("[REPORT_STORE] initialized=");
    out.print(status.initialized ? "yes" : "no");
    out.print(" available=");
    out.print(status.available ? "yes" : "no");
    out.print(" chunks_written=");
    out.print(static_cast<unsigned long>(status.chunks_written));
    out.print(" chunks_read=");
    out.print(static_cast<unsigned long>(status.chunks_read));
    out.print(" chunks_listed=");
    out.print(static_cast<unsigned long>(status.chunks_listed));
    out.print(" summary_written=");
    out.print(static_cast<unsigned long>(status.summary_records_written));
    out.print(" summary_read=");
    out.print(static_cast<unsigned long>(status.summary_records_read));
    out.print(" coverage_written=");
    out.print(static_cast<unsigned long>(status.coverage_records_written));
    out.print(" coverage_read=");
    out.print(static_cast<unsigned long>(status.coverage_records_read));
    out.print(" bytes_written=");
    print_uint64(out, status.bytes_written);
    out.print(" bytes_read=");
    print_uint64(out, status.bytes_read);
    out.print(" layout_errors=");
    out.print(static_cast<unsigned long>(status.layout_errors));
    out.print(" write_errors=");
    out.print(static_cast<unsigned long>(status.write_errors));
    out.print(" read_errors=");
    out.print(static_cast<unsigned long>(status.read_errors));
    out.print(" coverage_write_errors=");
    out.print(static_cast<unsigned long>(status.coverage_write_errors));
    out.print(" coverage_read_errors=");
    out.print(static_cast<unsigned long>(status.coverage_read_errors));
    out.print(" last_error=");
    out.print(status.last_error[0] ? status.last_error : "--");
    out.println();
}

void print_report_store_integrity(Print &out,
                                  const ReportStoreIntegrityResult &result) {
    out.print("[REPORT_STORE] integrity ");
    out.print(result.ok ? "ok" : "dirty");
    out.print(" repaired=");
    out.print(result.repaired ? "yes" : "no");
    out.print(" summary_invalid=");
    out.print(static_cast<unsigned long>(result.summary_invalid));
    out.print(" chunks_invalid=");
    out.print(static_cast<unsigned long>(result.chunks_invalid));
    out.print(" chunks_removed=");
    out.print(static_cast<unsigned long>(result.chunks_removed));
    out.print(" indexes_missing=");
    out.print(static_cast<unsigned long>(result.chunk_indexes_missing));
    out.print(" indexes_invalid=");
    out.print(static_cast<unsigned long>(result.chunk_indexes_invalid));
    out.print(" indexes_rebuilt=");
    out.print(static_cast<unsigned long>(result.chunk_indexes_rebuilt));
    out.print(" coverage_dropped=");
    out.print(static_cast<unsigned long>(result.coverage_records_dropped));
    out.print(" coverage_rewritten=");
    out.print(static_cast<unsigned long>(result.coverage_files_rewritten));
    out.print(" errors=");
    out.print(static_cast<unsigned long>(result.errors));
    out.print(" last_error=");
    out.print(result.last_error[0] ? result.last_error : "--");
    out.println();
}

void print_report_cache_clear_result(Print &out,
                                     const ReportCacheClearResult &result) {
    out.print("[REPORT] cache cleared reset=");
    out.print(static_cast<unsigned long>(result.store_reset));
    out.print(" summary=");
    out.print(static_cast<unsigned long>(result.summary_deleted));
    out.print(" nights=");
    out.print(static_cast<unsigned long>(result.nights_cleared));
    out.print(" chunks=");
    out.print(static_cast<unsigned long>(result.chunks_deleted));
    out.print(" coverage=");
    out.print(static_cast<unsigned long>(result.coverage_deleted));
    out.print(" plots=");
    out.print(static_cast<unsigned long>(result.plots_deleted));
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
    out.print(" rpc_observer=");
    out.print(status.rpc_observer_registered ? "yes" : "no");
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
    out.print(" mask_start=");
    out.print(status.mask_start_time[0] ? status.mask_start_time : "--");
    if (status.mask_start_pending()) {
        out.print(" mask_pending=yes");
    }
    out.print(" mask_events=");
    out.print(static_cast<unsigned long>(status.mask_start_events));
    out.print('/');
    out.print(static_cast<unsigned long>(status.mask_start_bad_events));
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
    const EdfStorageWorkerStatus storage = manager.storage_status();
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

const char *report_summary_state_name(ReportSummaryState state) {
    switch (state) {
        case ReportSummaryState::Fetching: return "fetching";
        case ReportSummaryState::Ready: return "ready";
        case ReportSummaryState::Error: return "error";
        case ReportSummaryState::Idle:
        default: return "idle";
    }
}

const char *report_result_state_name(ReportResultState state) {
    switch (state) {
        case ReportResultState::Preparing: return "preparing";
        case ReportResultState::Ready: return "ready";
        case ReportResultState::Incomplete: return "incomplete";
        case ReportResultState::Partial: return "partial";
        case ReportResultState::Error: return "error";
        case ReportResultState::Idle:
        default: return "idle";
    }
}

void print_duration_min(Print &out, uint32_t duration_min);

void print_report_summary_status(Print &out,
                                 const ReportManager &manager) {
    const ReportSummaryStatus status = manager.summary_status();
    out.print("[REPORT] summary=");
    out.print(report_summary_state_name(status.state));
    out.print(" revision=");
    out.print(static_cast<unsigned long>(status.revision));
    out.print(" records=");
    out.print(static_cast<unsigned long>(status.records_total));
    out.print(" therapy_nights=");
    out.print(static_cast<unsigned long>(status.nights_with_therapy));
    out.print(" elapsed_ms=");
    out.print(static_cast<unsigned long>(status.elapsed_ms));
    out.print(" active=");
    out.print(status.active_spool.length() ? status.active_spool.c_str()
                                           : "--");
    out.print(" error=");
    out.print(status.error.length() ? status.error.c_str() : "--");
    out.println();
}

void print_report_result_status(Print &out,
                                const ReportManager &manager) {
    const ReportResultStatus status = manager.result_status();
    out.print("[REPORT] result=");
    out.print(report_result_state_name(status.state));
    out.print(" index=");
    out.print(static_cast<unsigned long>(status.therapy_index));
    out.print(" night=");
    print_uint64(out, status.night_start_ms);
    out.print(" duration=");
    print_duration_min(out, status.duration_min);
    out.print(" missing_required=");
    out.print(static_cast<unsigned long>(status.missing_required));
    out.print(" missing_streams=");
    out.print(static_cast<unsigned long>(status.missing_streams));
    out.print(" streams=");
    out.print(static_cast<unsigned long>(status.stream_count));
    out.print(" chunks=");
    out.print(static_cast<unsigned long>(status.chunk_count));
    out.print(" records=");
    out.print(static_cast<unsigned long>(status.record_count));
    out.print(" bytes=");
    out.print(static_cast<unsigned long>(status.payload_bytes));
    out.print(" slots=");
    out.print(static_cast<unsigned long>(status.materialized_slots));
    out.print("/");
    out.print(static_cast<unsigned long>(status.materialized_plot_slots));
    out.print(" error=");
    out.print(status.error.length() ? status.error.c_str() : "--");
    out.println();

    const ReportManager::BuildQueueSnapshot queue =
        manager.build_queue_snapshot();
    out.print("[REPORT] build_queue=");
    if (!queue.available) {
        out.print("unavailable");
    } else if (!queue.lock_ok) {
        out.print("busy");
    } else {
        out.print(static_cast<unsigned long>(queue.count));
        if (queue.count > 0) {
            out.print(" head_index=");
            out.print(static_cast<unsigned long>(queue.head_therapy_index));
            out.print(" head_night=");
            print_uint64(out, queue.head_night_ms);
            out.print(" refresh=");
            out.print(queue.head_refresh ? "yes" : "no");
        }
        out.print(" enq=");
        out.print(static_cast<unsigned long>(queue.enqueue_total));
        out.print("/");
        out.print(static_cast<unsigned long>(queue.queued_total));
        out.print("/");
        out.print(static_cast<unsigned long>(queue.already_total));
        out.print(" svc=");
        out.print(static_cast<unsigned long>(queue.service_total));
        if (queue.last_read[0]) {
            out.print(" last_read=");
            out.print(queue.last_read);
        }
        if (queue.last_enqueue_result[0]) {
            out.print(" last_enqueue=");
            out.print(queue.last_enqueue_result);
            out.print("@");
            out.print(static_cast<unsigned long>(
                queue.last_enqueue_therapy_index));
            out.print("/");
            print_uint64(out, queue.last_enqueue_night_ms);
        }
        if (queue.last_service_block[0]) {
            out.print(" blocked=");
            out.print(queue.last_service_block);
        }
        if (queue.last_night_ms != 0 || queue.last_outcome[0]) {
            out.print(" last_index=");
            out.print(static_cast<unsigned long>(queue.last_therapy_index));
            out.print(" last_night=");
            print_uint64(out, queue.last_night_ms);
            out.print(" last_outcome=");
            out.print(queue.last_outcome[0] ? queue.last_outcome : "--");
            out.print(" last_state=");
            out.print(queue.last_state[0] ? queue.last_state : "--");
            out.print(" last_error=");
            out.print(queue.last_error[0] ? queue.last_error : "--");
        }
    }
    out.println();

    EdfReportCatalogStatus catalog;
    out.print("[REPORT] edf_catalog=");
    if (!manager.edf_catalog_status(catalog, 0)) {
        out.print("unavailable");
    } else {
        out.print(edf_report_catalog_state_name(catalog.state));
        out.print(" refresh=");
        out.print(static_cast<unsigned long>(catalog.refresh_id));
        out.print(" sessions=");
        out.print(static_cast<unsigned long>(catalog.sessions));
        out.print(" build_sessions=");
        out.print(static_cast<unsigned long>(catalog.build_sessions));
        out.print(" days=");
        out.print(static_cast<unsigned long>(catalog.days_scanned));
        out.print(" files=");
        out.print(static_cast<unsigned long>(catalog.files_scanned));
        out.print("/");
        out.print(static_cast<unsigned long>(catalog.files_indexed));
        out.print(" skipped=");
        out.print(static_cast<unsigned long>(catalog.files_skipped));
        out.print(" truncated=");
        out.print(catalog.truncated ? "yes" : "no");
        out.print(" current=");
        out.print(catalog.current_path[0] ? catalog.current_path : "--");
        out.print(" error=");
        out.print(catalog.error[0] ? catalog.error : "--");
    }
    out.println();
}

void print_report_prefetch_status(Print &out, const ReportManager &manager) {
    char line[128];
    BackgroundWorker *w = background_worker();
    if (w) {
        const BackgroundWorkerStatus s = w->status();
#if AC_STACK_PROFILE_ENABLED
        snprintf(line, sizeof(line),
                 "[REPORT] prefetch worker=%s gate=%s ticks=%lu stack_free=%lu",
                 s.enabled ? "enabled" : "disabled", s.gate_reason,
                 static_cast<unsigned long>(s.ticks),
                 static_cast<unsigned long>(s.stack_high_water_words));
#else
        snprintf(line, sizeof(line),
                 "[REPORT] prefetch worker=%s gate=%s ticks=%lu",
                 s.enabled ? "enabled" : "disabled", s.gate_reason,
                 static_cast<unsigned long>(s.ticks));
#endif
        out.println(line);
    } else {
        out.println("[REPORT] prefetch worker=unavailable");
    }
    const ReportManager::PrefetchSnapshot p = manager.prefetch_snapshot();
    const char *phase = "?";
    switch (p.phase) {
        case ReportManager::PrefetchPhase::Idle: phase = "idle"; break;
        case ReportManager::PrefetchPhase::Selecting: phase = "selecting"; break;
        case ReportManager::PrefetchPhase::Pending: phase = "pending"; break;
        case ReportManager::PrefetchPhase::Fetching: phase = "fetching"; break;
        case ReportManager::PrefetchPhase::Done: phase = "done"; break;
        case ReportManager::PrefetchPhase::Failed: phase = "failed"; break;
        case ReportManager::PrefetchPhase::Drained: phase = "drained"; break;
    }
    snprintf(line, sizeof(line),
             "[REPORT] prefetch phase=%s night_ms=%llu completed=%lu failed=%lu",
             phase, static_cast<unsigned long long>(p.night_ms),
             static_cast<unsigned long>(p.completed),
             static_cast<unsigned long>(p.failed));
    out.println(line);
    out.print("[REPORT] prefetch last_night=");
    print_uint64(out, p.last_night_ms);
    out.print(" last_failed=");
    print_uint64(out, p.last_failed_night_ms);
    out.print(" source=");
    out.print(p.last_source[0] ? p.last_source : "--");
    out.print(" error=");
    out.print(p.last_error[0] ? p.last_error : "--");
    out.println();
}

void print_report_cache_status(Print &out,
                               const ReportManager &manager) {
    const ReportCacheFetchStatus status = manager.cache_fetch_status();
    out.print("[REPORT] cache=");
    out.print(status.active ? "active" : "idle");
    out.print(" revision=");
    out.print(static_cast<unsigned long>(status.revision));
    out.print(" night=");
    print_uint64(out, status.night_start_ms);
    out.print(" source=");
    out.print(status.source_count ? report_source_spool_type(
                  status.active_source) : "--");
    out.print(" index=");
    out.print(static_cast<unsigned long>(status.source_index));
    out.print('/');
    out.print(static_cast<unsigned long>(status.source_count));
    out.print(" chunks=");
    out.print(static_cast<unsigned long>(status.chunks_written));
    out.print(" error=");
    out.print(status.error.length() ? status.error.c_str() : "--");
    out.println();
}

void print_utc_ms(Print &out, uint64_t ms) {
    const time_t seconds = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) {
        print_uint64(out, ms);
        return;
    }

    char buf[24];
    if (!strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv)) {
        print_uint64(out, ms);
        return;
    }
    out.print(buf);
}

void print_duration_min(Print &out, uint32_t duration_min) {
    out.print(static_cast<unsigned long>(duration_min / 60));
    out.print("h ");
    out.print(static_cast<unsigned long>(duration_min % 60));
    out.print('m');
}

struct ReportNightPrintContext {
    Print *out = nullptr;
    size_t count = 0;
};

bool print_report_night_row(void *context,
                            const ReportSummaryNight &night) {
    ReportNightPrintContext *ctx =
        static_cast<ReportNightPrintContext *>(context);
    if (!ctx || !ctx->out) return false;

    Print &out = *ctx->out;
    out.print("  ");
    out.print(static_cast<unsigned long>(night.therapy_index));
    out.print(": ");
    print_utc_ms(out, night.record.start_ms);
    out.print(" duration=");
    print_duration_min(out, night.record.duration_min);
    if (night.record.has_session_count) {
        out.print(" sessions=");
        out.print(static_cast<unsigned long>(night.record.session_count));
    }
    if (night.record.session_interval_count > 0) {
        out.print(" intervals=");
        out.print(static_cast<unsigned long>(
            night.record.session_interval_count));
    }
    out.print(" start_ms=");
    print_uint64(out, night.record.start_ms);
    out.println();
    ctx->count++;
    return true;
}

void print_report_nights(Print &out, const ReportManager &manager) {
    out.println("[REPORT nights]");
    ReportNightPrintContext ctx;
    ctx.out = &out;
    manager.for_each_summary_night(print_report_night_row, &ctx);
    if (!ctx.count) out.println("  no therapy nights indexed");
}

bool parse_u64_arg(const String &text, uint64_t &out) {
    if (!text.length()) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char ch = text.charAt(i);
        if (ch < '0' || ch > '9') return false;
        const uint8_t digit = static_cast<uint8_t>(ch - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_report_coverage_target(const String &arg,
                                  const ReportManager &manager,
                                  uint64_t &night_start_ms) {
    String value = arg;
    trim_inplace(value);
    to_lower_inplace(value);
    if (!value.length()) return false;

    if (value == "latest") {
        ReportSummaryRecord night;
        if (!manager.latest_summary_night(night)) return false;
        night_start_ms = night.start_ms;
        return true;
    }

    if (value.startsWith("ms ")) {
        value.remove(0, 3);
        trim_inplace(value);
        return parse_u64_arg(value, night_start_ms);
    }

    uint64_t numeric = 0;
    if (!parse_u64_arg(value, numeric)) return false;

    const ReportSummaryStatus status = manager.summary_status();
    if (numeric < status.nights_with_therapy) {
        ReportSummaryRecord night;
        if (!manager.summary_night_by_therapy_index(
                static_cast<size_t>(numeric), night)) {
            return false;
        }
        night_start_ms = night.start_ms;
        return true;
    }

    night_start_ms = numeric;
    return true;
}

void print_report_coverage(Print &out,
                           const ReportManager &manager,
                           uint64_t night_start_ms) {
    ReportNightCoverageStatus coverage;
    if (!manager.night_coverage(night_start_ms, coverage)) {
        out.println("[REPORT] coverage night not found");
        return;
    }
    out.print("[REPORT] coverage night=");
    print_uint64(out, coverage.start_ms);
    out.print(" end=");
    print_uint64(out, coverage.end_ms);
    out.print(" duration_min=");
    out.print(static_cast<unsigned long>(coverage.duration_min));
    out.print(" missing_required=");
    out.print(static_cast<unsigned long>(coverage.missing_required));
    out.println();
    for (size_t i = 0; i < coverage.source_count; ++i) {
        const ReportNightSourceCoverage &source = coverage.sources[i];
        out.print("  ");
        out.print(report_source_spool_type(source.source));
        out.print(" required=");
        out.print(source.required ? "yes" : "no");
        out.print(" coverage=");
        out.println(source.complete ? "complete" : "missing");
    }
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
    ConsoleFormat::print_rpc_status(out, ctx.arbiter);
    ConsoleFormat::print_as11_status(out, ctx.arbiter.as11_state());
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
        ctx.arbiter.reset_stats();
        out.println("[STATS] reset");
        return;
    }
    if (rest.length() && rest != "status") {
        print_unknown_command(out, "STATS", "stats, stats reset");
        return;
    }
    ConsoleFormat::print_rpc_stats(out, ctx.arbiter);
    ConsoleFormat::print_tcp_stats(out, ctx.tcp_bridge);
    ConsoleFormat::print_log_stats(out);
    ConsoleFormat::print_memory_status(out, Memory::status());
    ConsoleFormat::print_storage_status(out, Storage::status());
    ConsoleFormat::print_storage_writer_status(out, StorageWriter::status());
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
        print_report_summary_status(out, ctx.report_manager);
        print_report_cache_status(out, ctx.report_manager);
        print_report_result_status(out, ctx.report_manager);
        print_report_store_status(out);
        print_report_prefetch_status(out, ctx.report_manager);
        return;
    }
    if (rest == "store" || rest == "store status") {
        print_report_store_status(out);
        return;
    }
    if (rest == "store check" || rest == "store repair") {
        const char *reason = nullptr;
        if (!report_store_integrity_allowed(ctx, reason)) {
            out.print("[REPORT] store integrity scan refused: ");
            out.println(reason ? reason : "realtime_active");
            out.println("[REPORT] retry when therapy and live streams are idle");
            return;
        }
        const bool repair = rest == "store repair";
        ReportStoreIntegrityResult result;
        ReportStore::check_integrity(repair, result);
        print_report_store_integrity(out, result);
        print_report_store_status(out);
        return;
    }
    if (rest == "nights" || rest == "list") {
        print_report_nights(out, ctx.report_manager);
        return;
    }
    if (rest == "prefetch" || rest == "prefetch status") {
        print_report_prefetch_status(out, ctx.report_manager);
        return;
    }
    if (rest == "prefetch on" || rest == "prefetch off") {
        BackgroundWorker *w = background_worker();
        if (!w) {
            out.println("[REPORT] background worker unavailable");
            return;
        }
        const bool on = rest == "prefetch on";
        w->set_enabled(on);
        out.println(on ? "[REPORT] prefetch enabled"
                       : "[REPORT] prefetch disabled");
        print_report_prefetch_status(out, ctx.report_manager);
        return;
    }
    if (rest == "coverage") {
        out.println("[REPORT] usage: report coverage latest|INDEX|ms VALUE|NIGHT_START_MS");
        return;
    }
    if (rest == "cache") {
        out.println("[REPORT] usage: report cache [force] latest|INDEX|ms VALUE|NIGHT_START_MS");
        out.println("[REPORT] usage: report cache cancel");
        out.println("[REPORT] usage: report cache clear all|latest|INDEX|ms VALUE|NIGHT_START_MS");
        out.println("[REPORT] usage: report cache clear oldest N");
        out.println("[REPORT] usage: report cache prune [KEEP_LATEST]");
        out.println("[REPORT] usage: report store check|repair");
        return;
    }
    if (rest == "result") {
        print_report_result_status(out, ctx.report_manager);
        out.println("[REPORT] usage: report result latest|INDEX");
        return;
    }
    if (rest.startsWith("result ")) {
        String value = rest.substring(strlen("result "));
        trim_inplace(value);
        size_t index = 0;
        if (value == "latest") {
            index = 0;
        } else {
            uint64_t parsed = 0;
            if (!parse_u64_arg(value, parsed) ||
                parsed > static_cast<uint64_t>(SIZE_MAX)) {
                out.println("[REPORT] usage: report result latest|INDEX");
                return;
            }
            index = static_cast<size_t>(parsed);
        }
        if (!ctx.report_manager.prepare_result_by_therapy_index(index)) {
            out.println("[REPORT] result prepare failed");
        }
        print_report_result_status(out, ctx.report_manager);
        return;
    }
    if (rest.startsWith("cache ")) {
        String value = rest.substring(strlen("cache "));
        trim_inplace(value);
        if (value == "clear") {
            out.println("[REPORT] usage: report cache clear all|latest|INDEX|ms VALUE|NIGHT_START_MS");
            return;
        }
        if (value == "cancel") {
            if (!ctx.report_manager.cancel_cache_fetch()) {
                out.println("[REPORT] no active cache fetch");
            } else {
                out.println("[REPORT] cache fetch cancelled");
            }
            print_report_cache_status(out, ctx.report_manager);
            return;
        }
        if (value == "prune" || value.startsWith("prune ")) {
            size_t keep_latest = AC_REPORT_CACHE_QUOTA_NIGHTS;
            if (value.startsWith("prune ")) {
                value.remove(0, strlen("prune "));
                trim_inplace(value);
                uint64_t parsed = 0;
                if (!parse_u64_arg(value, parsed) ||
                    parsed > static_cast<uint64_t>(SIZE_MAX)) {
                    out.println("[REPORT] usage: report cache prune [KEEP_LATEST]");
                    return;
                }
                keep_latest = static_cast<size_t>(parsed);
            }
            ReportCacheClearResult clear_result;
            if (!ctx.report_manager.prune_cache_to_latest_nights(keep_latest,
                                                                  clear_result)) {
                out.println("[REPORT] cache prune rejected");
                print_report_cache_status(out, ctx.report_manager);
                return;
            }
            print_report_cache_clear_result(out, clear_result);
            print_report_store_status(out);
            return;
        }
        if (value.startsWith("clear ")) {
            value.remove(0, strlen("clear "));
            trim_inplace(value);
            ReportCacheClearResult clear_result;
            bool ok = false;
            if (value == "all") {
                ok = ctx.report_manager.clear_cache_all(clear_result);
            } else if (value.startsWith("oldest ")) {
                value.remove(0, strlen("oldest "));
                trim_inplace(value);
                uint64_t parsed = 0;
                if (!parse_u64_arg(value, parsed) ||
                    parsed > static_cast<uint64_t>(SIZE_MAX)) {
                    out.println("[REPORT] usage: report cache clear oldest N");
                    return;
                }
                ok = ctx.report_manager.clear_oldest_cache_nights(
                    static_cast<size_t>(parsed),
                    clear_result);
            } else {
                uint64_t night_start_ms = 0;
                if (!parse_report_coverage_target(value,
                                                  ctx.report_manager,
                                                  night_start_ms)) {
                    out.println("[REPORT] usage: report cache clear all|latest|INDEX|ms VALUE|NIGHT_START_MS");
                    return;
                }
                ok = ctx.report_manager.clear_cache_night(night_start_ms,
                                                          clear_result);
            }
            if (!ok) {
                out.println("[REPORT] cache clear rejected");
                print_report_cache_status(out, ctx.report_manager);
                return;
            }
            print_report_cache_clear_result(out, clear_result);
            print_report_store_status(out);
            return;
        }
        bool force = false;
        if (value.startsWith("force ")) {
            force = true;
            value.remove(0, strlen("force "));
            trim_inplace(value);
        }
        uint64_t night_start_ms = 0;
        if (!parse_report_coverage_target(value, ctx.report_manager,
                                          night_start_ms)) {
            out.println("[REPORT] usage: report cache [force] latest|INDEX|ms VALUE|NIGHT_START_MS");
            return;
        }
        if (!ctx.report_manager.request_night_cache(night_start_ms, force)) {
            out.println("[REPORT] cache request rejected");
            print_report_cache_status(out, ctx.report_manager);
            return;
        }
        out.println("[REPORT] cache request queued");
        print_report_cache_status(out, ctx.report_manager);
        return;
    }
    if (rest.startsWith("coverage ")) {
        String value = rest.substring(strlen("coverage "));
        trim_inplace(value);
        uint64_t night_start_ms = 0;
        if (!parse_report_coverage_target(value, ctx.report_manager,
                                          night_start_ms)) {
            out.println("[REPORT] usage: report coverage latest|INDEX|ms VALUE|NIGHT_START_MS");
            return;
        }
        print_report_coverage(out, ctx.report_manager, night_start_ms);
        return;
    }
    print_unknown_command(out, "REPORT",
                          "report, report status, report store, "
                          "report store check|repair, "
                          "report nights, report coverage latest|INDEX|ms VALUE, "
                          "report cache latest|INDEX|ms VALUE, "
                          "report cache cancel, "
                          "report cache clear all|oldest N|latest|INDEX|ms VALUE, "
                          "report cache prune [KEEP_LATEST], "
                          "report result latest|INDEX, "
                          "report prefetch [status|on|off]");
}

void ManagementConsole::handle_storage_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    (void)ctx;
    trim_inplace(rest);
    String rest_lower = rest;
    to_lower_inplace(rest_lower);
    if (!rest_lower.length() || rest_lower == "status") {
        ConsoleFormat::print_storage_status(out, Storage::status());
        ConsoleFormat::print_storage_writer_status(out,
                                                   StorageWriter::status());
        return;
    }
    if (rest_lower == "remount" || rest_lower == "retry") {
        Storage::remount();
        ConsoleFormat::print_storage_status(out, Storage::status());
        return;
    }
    if (rest_lower == "queue" || rest_lower == "writer") {
        ConsoleFormat::print_storage_writer_status(out,
                                                   StorageWriter::status());
        return;
    }
    if (rest_lower == "write-test" || rest_lower.startsWith("write-test ")) {
        String args = rest;
        args.remove(0, String("write-test").length());
        int pos = 0;
        String path = "/aircannect-write-test.txt";
        String text = "AirCANnect storage writer test";
        String parsed;
        if (parse_console_arg(args, pos, parsed)) {
            path = parsed;
            if (parse_console_arg(args, pos, parsed)) text = parsed;
        }
        text += '\n';
        const bool queued =
            StorageWriter::enqueue_append(path.c_str(),
                                          reinterpret_cast<const uint8_t *>(
                                              text.c_str()),
                                          text.length());
        out.print("[STORAGE_WRITER] test ");
        out.println(queued ? "queued" : "rejected");
        ConsoleFormat::print_storage_writer_status(out,
                                                   StorageWriter::status());
        return;
    }
    print_unknown_command(out, "STORAGE",
                          "storage status, remount, queue, write-test");
}

void ManagementConsole::handle_sleephq_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    trim_inplace(rest);
    to_lower_inplace(rest);
    SleepHqSyncJob *job = ctx.sleephq_sync_job;
    if (!job) {
        out.println("[SLEEPHQ] unavailable");
        return;
    }
    if (!rest.length() || rest == "status") {
        const SleepHqSyncStatus status = job->status();
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
        ExportCoordinator *coordinator = ctx.export_coordinator;
        if (!coordinator) {
            out.println("[SLEEPHQ] check rejected");
            return;
        }
        const bool queued = coordinator->request_sleephq_check();
        out.print("[SLEEPHQ] check ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    if (rest == "sync") {
        ExportCoordinator *coordinator = ctx.export_coordinator;
        if (!coordinator) {
            out.println("[SLEEPHQ] sync rejected");
            return;
        }
        const bool queued = coordinator->request_sleephq_sync();
        out.print("[SLEEPHQ] sync ");
        out.println(queued ? "queued" : "rejected");
        return;
    }
    print_unknown_command(out, "SLEEPHQ",
                          "sleephq status, sleephq check, sleephq sync");
}

void ManagementConsole::handle_as11_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    handle_as11(out, rest, ctx.arbiter);
}

void ManagementConsole::handle_therapy_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    handle_therapy(out, rest, ctx.arbiter);
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
    handle_ota(out, rest, ctx.ota_manager);
}

void ManagementConsole::handle_resmed_ota_command(Print &out,
                                                  String rest,
                                                  ConsoleContext &ctx) {
    handle_resmed_ota(out, rest, ctx.resmed_ota_manager);
}

void ManagementConsole::handle_log_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    handle_log(out, rest, ctx.app_config);
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
        ConsoleFormat::print_rpc_status(out, ctx.arbiter);
        return;
    }
    if (rest == "restart") {
        ctx.arbiter.recover_can("console CAN restart command");
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
    handle_time(out, rest, ctx.arbiter, ctx.time_sync_service);
}

void ManagementConsole::handle_get_command(Print &out,
                                           String rest,
                                           ConsoleContext &ctx) {
    trim_inplace(rest);
    if (!rest.length()) {
        out.println("[RPC] usage: get NAME [NAME...]");
        return;
    }
    ctx.arbiter.send_request("Get", build_get_params(to_std(rest)),
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

        const As11SettingsState &settings = ctx.arbiter.as11_settings();
        const As11DeviceState &as11 = ctx.arbiter.as11_state();
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

        bool first = true;
        params = "{";
        append_json_object_members(params, first, raw_params);
        append_json_object_members(params, first, mapped_params);
        params += "}";
    }

    if (ctx.arbiter.send_request("Set", params, RpcSource::Console)) {
        ctx.arbiter.request_as11_settings_refresh();
        out.println("[RPC] Set queued");
    } else {
        out.println("[RPC] Set queue failed");
    }
}

void ManagementConsole::handle_stream_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    handle_stream(out, rest, ctx.arbiter);
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
    ctx.arbiter.send_request(to_std(method), to_std(params),
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
    ctx.arbiter.submit_raw_payload(to_std(rest), RpcSource::Console);
}

}  // namespace aircannect
