#include "management_console_format.h"

#include <stdio.h>

#include "as11_device_state.h"

namespace aircannect {
namespace ConsoleFormat {
namespace {

void print_u64(Print &out, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    out.print(buf);
}

}  // namespace

void print_memory_status(Print &out, const MemoryStatus &mem) {
    out.print("[MEM] heap_free=");
    out.print(static_cast<unsigned long>(mem.heap_free));
    out.print(" heap_total=");
    out.print(static_cast<unsigned long>(mem.heap_total));
    out.print(" heap_max_alloc=");
    out.print(static_cast<unsigned long>(mem.heap_max_alloc));
    out.print(" psram=");
    out.print(mem.psram_available ? "yes" : "no");
    out.print(" psram_free=");
    out.print(static_cast<unsigned long>(mem.psram_free));
    out.print(" psram_total=");
    out.print(static_cast<unsigned long>(mem.psram_total));
    out.print(" psram_max_alloc=");
    out.print(static_cast<unsigned long>(mem.psram_max_alloc));
    out.println();
}

void print_storage_status(Print &out, const StorageStatus &s) {
    out.print("[STORAGE] configured=");
    out.print(s.configured ? "yes" : "no");
    out.print(" type=");
    out.print(Storage::type_name(s.type));
    out.print(" state=");
    out.print(Storage::state_name(s.state));
    out.print(" mounted=");
    out.print(s.mounted ? "yes" : "no");
    out.print(" card=");
    out.print(s.card_type);
    out.print(" width=");
    out.print(s.width);
    out.print(" mount=");
    out.print(s.mount_point);
    out.print(" total_bytes=");
    print_u64(out, s.total_bytes);
    out.print(" used_bytes=");
    print_u64(out, s.used_bytes);
    out.print(" free_bytes=");
    print_u64(out, s.free_bytes);
    if (s.last_error[0]) {
        out.print(" error=");
        out.print(s.last_error);
    }
    out.println();
}

void print_storage_writer_status(Print &out,
                                 const StorageWriterStatus &s) {
    out.print("[STORAGE_WRITER] initialized=");
    out.print(s.initialized ? "yes" : "no");
    out.print(" available=");
    out.print(s.available ? "yes" : "no");
    out.print(" psram=");
    out.print(s.using_psram ? "yes" : "no");
    out.print(" q=");
    out.print(s.queued);
    out.print('/');
    out.print(s.capacity);
    out.print(" chunk=");
    out.print(s.chunk_bytes);
    out.print(" enqueued=");
    out.print(s.enqueued);
    out.print(" written=");
    out.print(s.written);
    out.print(" bytes_enqueued=");
    print_u64(out, s.bytes_enqueued);
    out.print(" bytes_written=");
    print_u64(out, s.bytes_written);
    out.print(" q_drops=");
    out.print(s.queue_drops);
    out.print(" unavailable_drops=");
    out.print(s.unavailable_drops);
    out.print(" open_errors=");
    out.print(s.open_errors);
    out.print(" write_errors=");
    out.print(s.write_errors);
    if (s.last_path[0]) {
        out.print(" last_path=");
        out.print(s.last_path);
    }
    if (s.last_error[0]) {
        out.print(" error=");
        out.print(s.last_error);
    }
    out.println();
}

void print_session_status(Print &out, const SessionStatus &s) {
    out.print("[SESSION] state=");
    out.print(SessionManager::state_name(s.state));
    out.print(" id=");
    out.print(s.session_id);
    out.print(" therapy=");
    out.print(As11DeviceState::therapy_state_name(s.therapy_state));
    out.print(" starts=");
    out.print(s.start_count);
    out.print(" ends=");
    out.print(s.end_count);
    out.print(" frames=");
    out.print(s.frame_count);
    out.print(" drops=");
    out.print(s.dropped_frames);
    if (s.state == SessionState::Active && s.started_ms) {
        out.print(" age_ms=");
        out.print(millis() - s.started_ms);
    }
    if (s.last_frame_ms) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - s.last_frame_ms);
    }
    if (s.stream_id) {
        out.print(" stream_id=");
        out.print(s.stream_id);
    }
    if (s.start_device_time[0]) {
        out.print(" start_device_time=\"");
        out.print(s.start_device_time);
        out.print("\"");
    }
    if (s.end_device_time[0]) {
        out.print(" end_device_time=\"");
        out.print(s.end_device_time);
        out.print("\"");
    }
    if (s.end_reason[0]) {
        out.print(" end_reason=");
        out.print(s.end_reason);
    }
    out.println();
}

void print_sink_status(Print &out, const SinkManager &sink_manager) {
    const SinkRuntimeStatus &s = sink_manager.status();
    const LiveChartRuntimeStatus &live = sink_manager.live_chart_status();
    const DebugSinkRuntimeStatus debug = sink_manager.debug_status();

    out.print("[SINK] debug=");
    out.print(debug.enabled ? "on" : "off");
    out.print(" live=");
    out.print(live.enabled ? "on" : "off");
    out.print(" stream=");
    out.print(s.debug_stream_attached ? "attached" : "detached");
    out.print(" handle=");
    out.print(s.debug_stream_handle);
    out.print(" attach_attempts=");
    out.print(s.attach_attempts);
    out.print(" attach_failures=");
    out.print(s.attach_failures);
    out.print(" frames=");
    out.print(s.frames);
    out.print(" drops=");
    out.print(s.frame_drops);
    out.print(" sessions_started=");
    out.print(s.sessions_started);
    out.print(" sessions_ended=");
    out.print(s.sessions_ended);
    if (s.last_frame_ms) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - s.last_frame_ms);
    }
    if (s.last_error[0]) {
        out.print(" error=");
        out.print(s.last_error);
    }
    out.println();

    out.print("[SINK live] enabled=");
    out.print(live.enabled ? "yes" : "no");
    out.print(" desired=");
    out.print(live.desired ? "yes" : "no");
    out.print(" stream=");
    out.print(live.attached ? "attached" : "detached");
    out.print(" handle=");
    out.print(live.handle);
    out.print(" frames=");
    out.print(live.frames);
    out.print(" drops=");
    out.print(live.drops);
    out.print(" attach_failures=");
    out.print(live.attach_failures);
    if (live.last_frame_ms) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - live.last_frame_ms);
    }
    if (live.last_error[0]) {
        out.print(" error=");
        out.print(live.last_error);
    }
    out.println();

    out.print("[SINK debug] enabled=");
    out.print(debug.enabled ? "yes" : "no");
    out.print(" sessions_started=");
    out.print(debug.sessions_started);
    out.print(" sessions_ended=");
    out.print(debug.sessions_ended);
    out.print(" frames=");
    out.print(debug.frames);
    out.print(" last_session=");
    out.print(debug.last_session_id);
    if (debug.last_stream_id) {
        out.print(" last_stream=");
        out.print(debug.last_stream_id);
    }
    if (debug.last_frame_ms) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - debug.last_frame_ms);
    }
    out.println();
}

void print_tcp_status(Print &out, TcpBridge &tcp_bridge) {
    out.print("[TCP] started=");
    out.print(tcp_bridge.started() ? "yes" : "no");
    out.print(" port=");
    out.print(tcp_bridge.port());
    out.print(" clients=");
    out.println(tcp_bridge.connected_count());

    TcpBridgeClientStatus clients[AC_MAX_TCP_CLIENTS];
    const size_t count = tcp_bridge.client_statuses(clients,
                                                    AC_MAX_TCP_CLIENTS);
    for (size_t i = 0; i < count; ++i) {
        if (!clients[i].connected) continue;
        out.print("[TCP ");
        out.print(i);
        out.print("] remote=");
        out.print(clients[i].remote_ip);
        out.print(" line_buf=");
        out.print(clients[i].line_buffer_len);
        out.print(" out_q=");
        out.print(clients[i].output_queue_count);
        out.print(" out_current=");
        out.println(clients[i].output_current_len);
    }
}

void print_tcp_stats(Print &out, TcpBridge &tcp_bridge) {
    const TcpBridgeStats &stats = tcp_bridge.stats();
    const LineProtocolIoStats &io = tcp_bridge.io_stats();

    out.print(" tcp_started=");
    out.print(tcp_bridge.started() ? "yes" : "no");
    out.print(" tcp_port=");
    out.print(tcp_bridge.port());
    out.print(" tcp_clients=");
    out.print(tcp_bridge.connected_count());
    out.print(" tcp_accepted=");
    out.print(stats.accepted_clients);
    out.print(" tcp_disconnected=");
    out.print(stats.disconnected_clients);
    out.print(" tcp_broadcasts=");
    out.print(stats.broadcasts);
    out.print(" tcp_broadcast_targets=");
    out.print(stats.broadcast_targets);
    out.print(" tcp_broadcast_no_clients=");
    out.print(stats.broadcasts_without_clients);
    out.print(" tcp_in=");
    out.print(stats.lines_in);
    out.print(" tcp_bytes_in=");
    out.print(stats.bytes_in);
    out.print(" tcp_input_yields=");
    out.print(stats.input_yields);
    out.print(" tcp_out=");
    out.print(stats.lines_out);
    out.print(" tcp_bytes_out=");
    out.print(io.bytes_out);
    out.print(" tcp_write_attempts=");
    out.print(io.write_attempts);
    out.print(" tcp_write_deferred=");
    out.print(io.write_deferred);
    out.print(" tcp_write_zero=");
    out.print(io.write_zero);
    out.print(" tcp_write_errors=");
    out.print(io.write_errors);
    out.print(" tcp_overlong=");
    out.print(stats.overlong_lines);
    out.print(" tcp_enqueue_fail=");
    out.print(stats.enqueue_failures);
    out.print(" tcp_q_drops=");
    out.print(stats.queue_drops);
    out.print(" tcp_rejected=");
    out.print(stats.rejected_clients);
    out.print(" tcp_last_in_ms=");
    out.print(tcp_bridge.last_line_in_ms());
    out.print(" tcp_last_out_ms=");
    out.print(tcp_bridge.last_line_out_ms());
}

}  // namespace ConsoleFormat
}  // namespace aircannect
