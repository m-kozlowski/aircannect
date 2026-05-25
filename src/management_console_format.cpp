#include "management_console_format.h"

#include <stdio.h>

#include "as11_device_state.h"
#include "debug_log.h"

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

void print_can_status(Print &out, const CanDriver &can_driver) {
    CanControllerStatus status;
    if (!can_driver.controller_status(status)) {
        out.print("[CAN] status failed: ");
        out.println(CanDriver::error_name(status.error));
        return;
    }

    out.print("[CAN] state=");
    out.print(CanDriver::state_name(status.state));
    out.print(" tx_err=");
    out.print(status.tx_error_counter);
    out.print(" rx_err=");
    out.print(status.rx_error_counter);
    out.print(" tx_q=");
    out.print(status.msgs_to_tx);
    out.print(" rx_q=");
    out.print(status.msgs_to_rx);
    out.print(" tx_failed=");
    out.print(status.tx_failed_count);
    out.print(" rx_missed=");
    out.print(status.rx_missed_count);
    out.print(" rx_overrun=");
    out.print(status.rx_overrun_count);
    out.print(" arb_lost=");
    out.print(status.arb_lost_count);
    out.print(" bus_errors=");
    out.println(status.bus_error_count);
}

void print_can_stats(Print &out, const CanDriver &can_driver) {
    const CanDriverStats &stats = can_driver.stats();
    out.print(" can_rx_frames=");
    out.print(stats.rx_frames);
    out.print(" can_tx_frames=");
    out.print(stats.tx_frames);
    out.print(" can_tx_q=");
    out.print(can_driver.tx_queue_depth());
    out.print(" can_tx_q_drops=");
    out.print(stats.tx_queue_drops);
    out.print(" can_tx_failures=");
    out.print(stats.tx_failures);
    out.print(" recoveries=");
    out.print(stats.recoveries);
    out.print(" recovery_failures=");
    out.print(stats.recovery_failures);
    out.print(" bus_error_alerts=");
    out.print(stats.bus_error_alerts);
    out.print(" rx_queue_full_alerts=");
    out.print(stats.rx_queue_full_alerts);
}

void print_rpc_status(Print &out, const RpcArbiter &arbiter) {
    print_can_status(out, arbiter.can_driver());

    const RpcRuntimeStatus runtime = arbiter.runtime_status();
    if (runtime.last_boot_notification.empty()) {
        out.println("[BOOT] notifications=0");
        return;
    }

    out.print("[BOOT] notifications=");
    out.print(runtime.boot_notifications);
    out.print(" last_age_ms=");
    out.print(runtime.last_boot_notification_age_ms);
    out.print(" last=");
    out.println(runtime.last_boot_notification.c_str());
}

void print_rpc_stats(Print &out, const RpcArbiter &arbiter) {
    const RpcRuntimeStatus runtime = arbiter.runtime_status();
    const RpcArbiterStats &stats = arbiter.stats();
    const CanDriverStats &can_stats = arbiter.can_driver().stats();
    const StreamBroker &stream = arbiter.stream_broker();
    const uint32_t can_rx_fps =
        (can_stats.rx_frames * 1000UL) / runtime.stats_elapsed_ms;
    const uint32_t rpc_dps =
        (stats.rpc_datagrams * 1000UL) / runtime.stats_elapsed_ms;

    out.print("[STATS]");
    out.print(" elapsed_ms=");
    out.print(runtime.stats_elapsed_ms);
    print_can_stats(out, arbiter.can_driver());
    out.print(" can_rx_fps=");
    out.print(can_rx_fps);
    out.print(" rpc_datagrams=");
    out.print(stats.rpc_datagrams);
    out.print(" rpc_dps=");
    out.print(rpc_dps);
    out.print(" responses=");
    out.print(stats.rpc_responses);
    out.print(" notifications=");
    out.print(stats.rpc_notifications);
    out.print(" unmatched=");
    out.print(stats.rpc_unmatched);
    out.print(" rpc_framing_errors=");
    out.print(stats.rpc_framing_errors);
    out.print(" log_datagrams=");
    out.print(stats.log_datagrams);
    out.print(" log_framing_errors=");
    out.print(stats.log_framing_errors);
    out.print(" boot_notifications=");
    out.print(runtime.boot_notifications);
    out.print(" rpc_req_q=");
    out.print(runtime.request_queue_depth);
    out.print(" rpc_pending=");
    out.print(runtime.pending_request_id);
    out.print(" rpc_dispatch_retry=");
    out.print(runtime.dispatch_retry_id);
    out.print(" queued_requests=");
    out.print(stats.queued_requests);
    out.print(" dispatched_requests=");
    out.print(stats.dispatched_requests);
    out.print(" request_timeouts=");
    out.print(stats.request_timeouts);
    out.print(" request_q_drops=");
    out.print(stats.request_queue_drops);
    out.print(" request_cancellations=");
    out.print(stats.request_cancellations);
    out.print(" request_dispatch_retries=");
    out.print(stats.request_dispatch_retries);
    out.print(" background_backoffs=");
    out.print(stats.background_backoffs);
    out.print(" background_backoff_ms=");
    out.print(runtime.background_backoff_ms);
    out.print(" event_drops=");
    out.print(stats.event_drops);
    out.print(" stream_consumers=");
    out.print(stream.consumer_count());
    out.print(" stream_subscribed=");
    out.print(stream.actual_active() ? "yes" : "no");
    out.print(" stream_start_pending=");
    out.print(stream.pending_start() ? "yes" : "no");
    out.print(" stream_stop_pending=");
    out.print(stream.pending_stop() ? "yes" : "no");
    out.print(" stream_starts=");
    out.print(stats.stream_start_requests);
    out.print(" stream_stops=");
    out.print(stats.stream_stop_requests);
    out.print(" stream_notifications=");
    out.print(stats.stream_notifications);
    out.print(" stream_fanout_drops=");
    out.print(stats.stream_fanout_drops);
    out.print(" stream_rejects=");
    out.print(stats.stream_consumer_rejects);
    out.print(" stream_deferred=");
    out.print(stats.stream_command_deferred);
    out.print(" stream_errors=");
    out.print(stats.stream_command_errors);
    out.print(" stream_parse_errors=");
    out.print(stats.stream_parse_errors);
    out.print(" stream_pool_exhaustions=");
    out.print(stats.stream_pool_exhaustions);
    out.print(" stream_truncated_frames=");
    out.print(stats.stream_truncated_frames);
    out.print(" stream_frame_pool=");
    out.print(stream.frame_pool_in_use());
    out.print("/");
    out.print(stream.frame_pool_capacity());
    out.print(" event_subscribed=");
    out.print(runtime.event_subscription_active ? "yes" : "no");
    out.print(" event_subscription_id=");
    out.print(runtime.event_subscription_id);
    out.print(" event_subscribe_errors=");
    out.print(stats.event_subscribe_errors);
    out.print(" event_notifications=");
    out.print(stats.event_notifications);
    out.print(" activity_state_events=");
    out.print(stats.activity_state_events);
}

void print_as11_status(Print &out, const As11DeviceState &state) {
    out.print("[AS11] status=");
    out.print(state.status_valid() ? "known" : "unknown");
    if (state.status_valid()) {
        out.print(" age_ms=");
        out.print(millis() - state.status_updated_ms());
    }
    out.println();

    out.print("[AS11] name=\"");
    out.print(state.product_name().c_str());
    out.print("\" serial=\"");
    out.print(state.serial_number().c_str());
    out.print("\" sid=\"");
    out.print(state.software_identifier().c_str());
    out.println("\"");

    out.print("[AS11] therapy=");
    out.print(As11DeviceState::therapy_state_name(state.therapy_state()));
    out.print(" pending=");
    out.print(As11DeviceState::therapy_target_name(
        state.pending_therapy_target()));
    if (!state.last_therapy_command_status().empty()) {
        out.print(" command=");
        out.print(state.last_therapy_command_status().c_str());
    }
    out.print(" rop=\"");
    out.print(state.rop().c_str());
    out.print("\" mode=\"");
    out.print(state.active_therapy_profile().c_str());
    out.print("\" activity_event=\"");
    out.print(state.last_activity_event().c_str());
    out.print("\"");
    if (state.last_activity_event_ms()) {
        out.print(" event_age_ms=");
        out.print(millis() - state.last_activity_event_ms());
    }
    out.println();

    out.print("[AS11] mhr=\"");
    out.print(state.mhr().c_str());
    out.print("\"");
    if (state.timezone_offset_valid()) {
        out.print(" timezone_offset_min=");
        out.print(state.timezone_offset_minutes());
    } else {
        out.print(" timezone_offset_min=unknown");
    }
    out.println();

    out.print("[AS11] datetime=\"");
    out.print(state.device_datetime().c_str());
    out.print("\" clock=");
    out.print(state.clock_valid() ? "known" : "unknown");
    if (state.clock_valid()) {
        out.print(" age_ms=");
        out.print(millis() - state.clock_sample_ms());
    }
    out.print(" offset_ms=");
    if (state.clock_offset_valid()) {
        out.print(state.clock_offset_ms());
    } else {
        out.print("unknown");
    }
    out.println();
}

void print_stream_status(Print &out, const RpcArbiter &arbiter) {
    const RpcArbiterStats &stats = arbiter.stats();
    const StreamBroker &stream = arbiter.stream_broker();

    out.print("[STREAM] consumers=");
    out.print(stream.consumer_count());
    out.print(" subscribed=");
    out.print(stream.actual_active() ? "yes" : "no");
    out.print(" start_pending=");
    out.print(stream.pending_start() ? "yes" : "no");
    out.print(" stop_pending=");
    out.print(stream.pending_stop() ? "yes" : "no");
    out.print(" error=");
    out.print(stream.error() ? "yes" : "no");
    out.print(" notifications=");
    out.print(stats.stream_notifications);
    if (stream.last_notification_ms()) {
        out.print(" last_age_ms=");
        out.print(millis() - stream.last_notification_ms());
    }
    if (stream.last_stream_id()) {
        out.print(" stream_id=");
        out.print(stream.last_stream_id());
    }
    if (!stream.last_start_time().empty()) {
        out.print(" startTime=\"");
        out.print(stream.last_start_time().c_str());
        out.print("\"");
    }
    out.println();
    if (!stream.params_json().empty()) {
        out.print("[STREAM] params=");
        out.println(stream.params_json().c_str());
    }
    out.print("[STREAM] frame_pool used=");
    out.print(stream.frame_pool_in_use());
    out.print(" free=");
    out.print(stream.frame_pool_free());
    out.print(" capacity=");
    out.print(stream.frame_pool_capacity());
    out.print(" parse_errors=");
    out.print(stream.parse_errors());
    out.print(" pool_exhaustions=");
    out.print(stream.pool_exhaustions());
    out.print(" truncated=");
    out.println(stream.truncated_frames());
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        StreamConsumerHandle handle = static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;
        out.print("[STREAM consumer ");
        out.print(i);
        out.print("] source=");
        out.print(stream.consumer_source(handle));
        out.print(" q=");
        out.print(stream.consumer_queue_count(handle));
        out.print(" drops=");
        out.println(stream.consumer_queue_drops(handle));
    }
}

void print_log_status(Print &out) {
    out.print("[LOG] levels");
    for (int i = 0; i < CAT_COUNT; ++i) {
        const log_cat_t cat = static_cast<log_cat_t>(i);
        out.print(' ');
        out.print(Log::cat_name(cat));
        out.print('=');
        out.print(Log::level_name(Log::get_cat_level(cat)));
    }
    out.println();

    const Log::Stats stats = Log::stats();
    const String host = Log::syslog_host();
    out.print("[SYSLOG] enabled=");
    out.print(Log::syslog_enabled() ? "yes" : "no");
    out.print(" host=");
    out.print(host.length() ? host : "--");
    out.print(" port=");
    out.print(Log::syslog_port());
    out.print(" queued=");
    out.print(Log::syslog_queue_depth());
    out.print(" sent=");
    out.print(stats.syslog_sent);
    out.print(" drops=");
    out.print(stats.syslog_drops);
    out.print(" errors=");
    out.println(stats.syslog_errors);
}

void print_log_stats(Print &out) {
    const Log::Stats stats = Log::stats();
    out.print(" log_emitted=");
    out.print(stats.emitted);
    out.print(" log_filtered=");
    out.print(stats.filtered);
    out.print(" log_truncated=");
    out.print(stats.truncated);
    out.print(" syslog_enabled=");
    out.print(Log::syslog_enabled() ? "yes" : "no");
    out.print(" syslog_q=");
    out.print(Log::syslog_queue_depth());
    out.print(" syslog_enqueued=");
    out.print(stats.syslog_enqueued);
    out.print(" syslog_sent=");
    out.print(stats.syslog_sent);
    out.print(" syslog_drops=");
    out.print(stats.syslog_drops);
    out.print(" syslog_errors=");
    out.print(stats.syslog_errors);
}

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

void print_memory_region(Print &out,
                         const char *name,
                         const MemoryRegionStatus &region) {
    out.print("[MEM region] name=");
    out.print(name);
    out.print(" free=");
    out.print(static_cast<unsigned long>(region.free_bytes));
    out.print(" allocated=");
    out.print(static_cast<unsigned long>(region.allocated_bytes));
    out.print(" largest=");
    out.print(static_cast<unsigned long>(region.largest_free_block));
    out.print(" min_free=");
    out.print(static_cast<unsigned long>(region.minimum_free_bytes));
    out.print(" blocks_alloc=");
    out.print(static_cast<unsigned long>(region.allocated_blocks));
    out.print(" blocks_free=");
    out.print(static_cast<unsigned long>(region.free_blocks));
    out.print(" blocks_total=");
    out.print(static_cast<unsigned long>(region.total_blocks));
    out.println();
}

void print_memory_detail_status(Print &out,
                                const MemoryDetailStatus &detail) {
    print_memory_status(out, detail.summary);
    print_memory_region(out, "default_8bit", detail.default_8bit);
    print_memory_region(out, "internal_8bit", detail.internal_8bit);
    print_memory_region(out, "internal_dma", detail.internal_dma);
    if (detail.summary.psram_available) {
        print_memory_region(out, "psram_8bit", detail.psram_8bit);
    }
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
    out.print(" tcp_in=");
    out.print(stats.lines_in);
    out.print(" tcp_bytes_in=");
    out.print(stats.bytes_in);
    out.print(" tcp_out=");
    out.print(stats.lines_out);
    out.print(" tcp_bytes_out=");
    out.print(io.bytes_out);
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
}

void print_wifi_status(Print &out, const WifiManager &wifi_manager) {
    out.print("[WiFi] mode=");
    out.print(wifi_manager.state_name());
    out.print(" configured=");
    out.print(wifi_manager.has_sta_config() ? "yes" : "no");
    out.print(" hostname=\"");
    out.print(wifi_manager.hostname());
    out.print("\" softap_mode=");
    out.print(softap_mode_name(wifi_manager.softap_mode()));
    out.print(" softap=");
    out.print(wifi_manager.softap_running() ? "up" : "down");
    out.print(" roaming=");
    out.print(wifi_manager.roaming_enabled()
                  ? (wifi_manager.roaming_suspended() ? "suspended" : "on")
                  : "off");
    out.print(" country=\"");
    out.print(wifi_manager.country_code());
    out.print("\"");
    if (wifi_manager.has_sta_config()) {
        out.print(" profiles=");
        out.print(wifi_manager.profile_count());
    }
    const int8_t active = wifi_manager.active_profile_index();
    if (active >= 0 &&
        active < static_cast<int8_t>(wifi_manager.profile_count())) {
        out.print(" ssid=\"");
        out.print(wifi_manager.sta_ssid());
        out.print("\" auth=");
        out.print(wifi_manager.sta_is_open() ? "open" : "password");
        out.print(" active_profile=");
        out.print(static_cast<int>(active));
    }
    out.print(" ip=");
    out.print(wifi_manager.ip());
    if (wifi_manager.softap_running()) {
        out.print(" ap_ip=");
        out.print(wifi_manager.softap_ip());
    }
    if (wifi_manager.mode_state() == WifiModeState::StaConnected) {
        out.print(" gw=");
        out.print(wifi_manager.gateway());
        out.print(" rssi=");
        out.print(wifi_manager.rssi());
        out.print(" bssid=");
        char bssid_text[AC_WIFI_BSSID_TEXT_MAX];
        wifi_manager.bssid(bssid_text, sizeof(bssid_text));
        out.print(bssid_text);
        out.print(" channel=");
        out.print(wifi_manager.channel());
    } else if (wifi_manager.mode_state() == WifiModeState::StaRoamScanning) {
        out.print(" roam_scan=running rssi=");
        out.print(wifi_manager.rssi());
    } else if (wifi_manager.mode_state() == WifiModeState::StaConnecting ||
               wifi_manager.mode_state() == WifiModeState::StaPmfRetry) {
        out.print(" timeout_ms=");
        out.print(wifi_manager.connect_timeout_remaining_ms());
    }

    const WifiManagerStats &stats = wifi_manager.stats();
    out.print(" attempts=");
    out.print(stats.connect_attempts);
    out.print(" successes=");
    out.print(stats.connect_successes);
    out.print(" failures=");
    out.print(stats.connect_failures);
    out.print(" disconnects=");
    out.print(stats.disconnects);
    out.print(" pmf_retries=");
    out.print(stats.pmf_retries);
    out.print(" roam_scans=");
    out.print(stats.roam_scans);
    out.print(" roam_switches=");
    out.print(stats.roam_switches);
    out.print(" roam_candidates=");
    out.print(stats.last_roam_candidates);
    out.print(" last_reason=");
    out.print(stats.last_disconnect_reason);
    out.println();
}

}  // namespace ConsoleFormat
}  // namespace aircannect
