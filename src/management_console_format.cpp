#include "management_console_format.h"

#include <stdio.h>
#include <time.h>

#include "as11_device_state.h"
#include "board.h"
#include "debug_log.h"
#include "event_broker.h"
#include "time_sync_service.h"

namespace aircannect {
namespace ConsoleFormat {
namespace {

void print_u64(Print &out, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    out.print(buf);
}

void print_can_stats(Print &out,
                     const CanDriver &can_driver,
                     uint32_t rx_fps) {
    const CanDriverStats &stats = can_driver.stats();

    out.print("[CAN traffic] rx_frames=");
    out.print(stats.rx_frames);
    out.print(" rx_fps=");
    out.print(rx_fps);
    out.print(" tx_frames=");
    out.print(stats.tx_frames);
    out.print(" tx_q=");
    out.print(can_driver.tx_queue_depth());
    out.print(" tx_q_drops=");
    out.print(stats.tx_queue_drops);
    out.print(" tx_failures=");
    out.println(stats.tx_failures);

    out.print("[CAN recovery] recoveries=");
    out.print(stats.recoveries);
    out.print(" failures=");
    out.print(stats.recovery_failures);
    out.print(" timeouts=");
    out.print(stats.recovery_timeouts);
    out.print(" driver_reinstalls=");
    out.println(stats.driver_reinstalls);

    out.print("[CAN alerts] bus_errors=");
    out.print(stats.bus_error_alerts);
    out.print(" rx_queue_full=");
    out.println(stats.rx_queue_full_alerts);
}

}  // namespace

void print_can_status(Print &out, const CanDriver &can_driver) {
    if (!can_driver.started()) {
        out.println("[CAN] state=disabled");
        return;
    }

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
    out.print(status.bus_error_count);
    if (status.recovery_active) {
        out.print(" recovery_age_ms=");
        out.print(status.recovery_age_ms);
        out.print(" recovery_attempts=");
        out.print(status.recovery_attempts);
        out.print(" restart_attempts=");
        out.print(status.restart_attempts);
    }
    out.println();
}

void print_rpc_status(Print &out,
                      const RpcDiagnosticsPort &rpc,
                      const CanDriver &can_driver) {
    print_can_status(out, can_driver);

    const RpcTransportStatus runtime = rpc.runtime_status();
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

void print_rpc_stats(Print &out,
                     const RpcDiagnosticsPort &rpc,
                     const CanDriver &can_driver,
                     const EventBroker &events,
                     const StreamBroker &stream) {
    const RpcTransportStatus runtime = rpc.runtime_status();
    const RpcTransportStats stats = rpc.stats();
    const EventBrokerStatus event_status = events.status();
    const EventBrokerStats &event_stats = events.stats();
    const CanDriverStats &can_stats = can_driver.stats();
    const uint32_t elapsed_ms = runtime.stats_elapsed_ms;
    const uint32_t can_rx_fps = elapsed_ms
        ? static_cast<uint32_t>(
              (static_cast<uint64_t>(can_stats.rx_frames) * 1000ULL) /
              elapsed_ms)
        : 0;
    const uint32_t rpc_dps = elapsed_ms
        ? static_cast<uint32_t>(
              (static_cast<uint64_t>(stats.rpc_datagrams) * 1000ULL) /
              elapsed_ms)
        : 0;

    out.print("[STATS] elapsed_ms=");
    out.println(elapsed_ms);

    print_can_stats(out, can_driver, can_rx_fps);

    out.print("[RPC stats] datagrams=");
    out.print(stats.rpc_datagrams);
    out.print(" dps=");
    out.print(rpc_dps);
    out.print(" responses=");
    out.print(stats.rpc_responses);
    out.print(" notifications=");
    out.print(stats.rpc_notifications);
    out.print(" unmatched=");
    out.print(stats.rpc_unmatched);
    out.print(" framing_errors=");
    out.println(stats.rpc_framing_errors);

    out.print("[RPC payload] drops=");
    out.print(stats.deferred_payload_drops);
    out.print(" alloc_failures=");
    out.println(stats.deferred_payload_alloc_failures);

    out.print("[RPC queue] requests=");
    out.print(runtime.request_queue_depth);
    out.print(" payloads=");
    out.print(runtime.payload_queue_depth);
    out.print(" pending=");
    out.print(runtime.pending_request_id);
    out.print(" dispatch_retry=");
    out.print(runtime.dispatch_retry_id);
    out.print(" dispatched=");
    out.println(stats.dispatched_requests);

    out.print("[RPC faults] timeouts=");
    out.print(stats.request_timeouts);
    out.print(" queue_drops=");
    out.print(stats.request_queue_drops);
    out.print(" cancellations=");
    out.print(stats.request_cancellations);
    out.print(" late_responses=");
    out.print(stats.late_addressed_responses);
    out.print(" dispatch_retries=");
    out.print(stats.request_dispatch_retries);
    out.print(" log_framing_errors=");
    out.print(stats.log_framing_errors);
    out.print(" rx_pressure_backoff_ms=");
    out.println(runtime.rx_pressure_backoff_ms);

    out.print("[STREAM state] consumers=");
    out.print(stream.consumer_count());
    out.print(" subscribed=");
    out.print(stream.actual_active() ? "yes" : "no");
    out.print(" start_pending=");
    out.print(stream.pending_start() ? "yes" : "no");
    out.print(" stop_pending=");
    out.print(stream.pending_stop() ? "yes" : "no");
    out.println();

    out.print("[STREAM data] payloads=");
    out.print(stream.published_payloads());
    out.print(" fanout_drops=");
    out.print(stream.total_queue_drops());
    out.print(" errors=");
    out.print(stream.command_errors());
    out.print(" parse_errors=");
    out.print(stream.parse_errors());
    out.print(" pool_exhaustions=");
    out.print(stream.pool_exhaustions());
    out.print(" truncated_frames=");
    out.print(stream.truncated_frames());
    out.print(" frame_pool=");
    out.print(stream.frame_pool_in_use());
    out.print('/');
    out.print(stream.frame_pool_capacity());
    out.print(" pool_alloc_failures=");
    out.println(stream.frame_pool_allocation_failures());

    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        const StreamConsumerHandle handle =
            static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;

        out.print("[STREAM consumer] id=");
        out.print(i);
        out.print(" source=");
        out.print(static_cast<unsigned>(stream.consumer_source(handle)));
        out.print(" queue=");
        out.print(stream.consumer_queue_count(handle));
        out.print('/');
        out.print(AC_STREAM_CONSUMER_QUEUE_DEPTH);
        out.print(" drops=");
        out.println(stream.consumer_queue_drops(handle));
    }

    out.print("[EVENT stats] subscribed=");
    out.print(event_status.subscription_active ? "yes" : "no");
    out.print(" subscription_id=");
    out.print(event_status.subscription_id);
    out.print(" command_errors=");
    out.print(event_stats.command_errors);
    out.print(" notifications=");
    out.print(event_stats.notifications);
    out.print(" truncated=");
    out.print(event_stats.truncated_notifications);
    out.print(" queue_drops=");
    out.println(stats.event_drops);
}

void print_as11_summary(Print &out,
                        const As11DeviceState &state,
                        As11Transport transport) {
    out.print("[AS11] state=");
    out.print(As11DeviceState::availability_name(state.availability()));
    out.print(" transport=");
    out.print(as11_transport_name(transport));
    out.print(" therapy=");
    out.print(As11DeviceState::therapy_state_name(state.therapy_state()));
    if (!state.active_therapy_profile().empty()) {
        out.print(" mode=\"");
        out.print(state.active_therapy_profile().c_str());
        out.print('"');
    }
    if (state.therapy_command_pending()) {
        out.print(" pending=");
        out.print(As11DeviceState::therapy_target_name(
            state.pending_therapy_target()));
    }
    out.println();

    if (state.product_name().empty() && state.serial_number().empty() &&
        state.software_identifier().empty()) {
        return;
    }

    out.print("[AS11] name=\"");
    out.print(state.product_name().c_str());
    out.print("\" serial=\"");
    out.print(state.serial_number().c_str());
    out.print("\" sid=\"");
    out.print(state.software_identifier().c_str());
    out.println("\"");
}

void print_as11_status(Print &out,
                       const As11DeviceState &state,
                       As11Transport transport) {
    out.print("[AS11] state=");
    out.print(As11DeviceState::availability_name(state.availability()));
    out.print(" transport=");
    out.print(as11_transport_name(transport));
    out.print(" status=");
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
    if (state.timezone_offset_valid()) {
        out.print(" timezone_offset_min=");
        out.print(state.timezone_offset_minutes());
    } else {
        out.print(" timezone_offset_min=unknown");
    }
    out.println();
}

void print_time_summary(Print &out,
                        const As11DeviceState &state,
                        const TimeSyncService &time_sync) {
    const time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    char local_text[24];
    strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S", &local);

    out.print("[TIME] local=\"");
    out.print(time_sync.esp_clock_valid() ? local_text : "invalid");
    out.print("\" source=");
    out.print(time_sync.esp_clock_source_name());
    out.print(" as11_offset_ms=");
    if (state.clock_offset_valid()) {
        out.print(state.clock_offset_ms());
    } else {
        out.print("unknown");
    }
    out.println();
}

void print_time_status(Print &out,
                       const As11DeviceState &state,
                       const TimeSyncService &time_sync) {
    const time_t now = time(nullptr);
    struct tm utc = {};
    struct tm local = {};
    gmtime_r(&now, &utc);
    localtime_r(&now, &local);
    char utc_text[24];
    char local_text[24];
    strftime(utc_text, sizeof(utc_text), "%Y-%m-%d %H:%M:%S", &utc);
    strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S", &local);

    out.print("[TIME] utc=");
    out.print(time_sync.esp_clock_valid() ? utc_text : "invalid");
    out.print(" local=");
    out.print(time_sync.esp_clock_valid() ? local_text : "invalid");
    out.print(" epoch=");
    out.print(static_cast<uint32_t>(now));
    out.print(" source=");
    out.print(time_sync.esp_clock_source_name());
    out.print(" ntp=");
    out.print(time_sync.ntp_synced() ? "synced" : "not_synced");
    out.print(" resmed_push=");
    out.print(time_sync.resmed_time_sync_enabled() ? "on" : "off");
    out.print(" resmed_offset_ms=");
    if (state.clock_offset_valid()) {
        out.print(state.clock_offset_ms());
    } else {
        out.print("unknown");
    }
    out.print(" status=");
    out.println(time_sync.last_status());
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

    out.print("[FILELOG] enabled=");
    out.print(Log::filelog_enabled() ? "yes" : "no");
    out.print(" path=");
    out.print(AC_FILE_LOG_PATH);
    out.print(" queued=");
    out.print(Log::filelog_queue_depth());
    out.print(" drops=");
    out.print(stats.file_drops);
    out.print(" errors=");
    out.println(stats.file_errors);
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
    out.print(" max_open=");
    out.print(s.max_open_files);
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
    if (s.recovered_active_start) {
        out.print(" recovered=yes");
    }
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

void print_session_summary(Print &out, const SessionStatus &s) {
    out.print("[SESSION] state=");
    out.print(SessionManager::state_name(s.state));
    if (s.state == SessionState::Active) {
        if (s.start_device_time[0]) {
            out.print(" started=\"");
            out.print(s.start_device_time);
            out.print('"');
        }
    } else if (s.end_device_time[0]) {
        out.print(" last_end=\"");
        out.print(s.end_device_time);
        out.print('"');
        if (s.end_reason[0]) {
            out.print(" reason=");
            out.print(s.end_reason);
        }
    }
    out.println();
}

void print_therapy_telemetry_status(
    Print &out,
    const TherapyTelemetryRuntimeStatus &status) {
    out.print("[TELEMETRY] desired=");
    out.print(status.desired ? "yes" : "no");
    out.print(" stream=");
    out.print(status.attached ? "attached" : "detached");
    out.print(" handle=");
    out.print(status.stream_handle);
    out.print(" subscribers=");
    out.print(status.subscribers);
    out.print(" metrics=");

    const char *separator = "";
    const auto print_metric = [&](TherapyTelemetryMetric metric, const char *name) {
        if (!(status.requested_metrics & metric)) return;
        out.print(separator);
        out.print(name);
        separator = ",";
    };

    print_metric(THERAPY_METRIC_PRESSURE, "pressure");
    print_metric(THERAPY_METRIC_LEAK, "leak");
    print_metric(THERAPY_METRIC_TIDAL_VOLUME, "tidal_volume");
    print_metric(THERAPY_METRIC_RESPIRATORY_RATE, "respiratory_rate");
    print_metric(THERAPY_METRIC_MINUTE_VENTILATION, "minute_ventilation");
    print_metric(THERAPY_METRIC_FLOW_LIMITATION, "flow_limitation");
    print_metric(THERAPY_METRIC_INSPIRATORY_DURATION, "inspiratory_duration");
    print_metric(THERAPY_METRIC_IE_RATIO, "ie_ratio");
    print_metric(THERAPY_METRIC_SNORE, "snore");
    if (!separator[0]) out.print("--");
    out.print(" drops=");
    out.print(status.queue_drops);
    out.print(" attach_failures=");
    out.print(status.attach_failures);
    if (status.last_frame_ms) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - status.last_frame_ms);
    }
    if (status.last_error[0]) {
        out.print(" error=");
        out.print(status.last_error);
    }
    out.println();
}

void print_live_stats(Print &out, const LiveChartService &live_service) {
    const LiveChartRuntimeStatus &live = live_service.status();

    out.print("[LIVE stats] frames=");
    out.print(live.frames);
    out.print(" drops=");
    out.print(live.drops);
    out.print(" attach_failures=");
    out.println(live.attach_failures);
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
    if (wifi_manager.mode_state() == WifiModeState::StaConnected ||
        wifi_manager.mode_state() == WifiModeState::StaAssociated) {
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
        if (wifi_manager.mode_state() == WifiModeState::StaAssociated) {
            out.print(" ipv4_timeout_ms=");
            out.print(wifi_manager.ipv4_timeout_remaining_ms());
        }
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
    out.print(" ipv4_timeouts=");
    out.print(stats.ipv4_timeouts);
    out.print(" ip_failed=");
    out.print(stats.ipv4_failed_candidates);
    out.print(" ip_skip=");
    out.print(stats.last_ip_failed_skips);
    out.print(" last_reason=");
    out.print(stats.last_disconnect_reason);
    out.println();
}

}  // namespace ConsoleFormat
}  // namespace aircannect
