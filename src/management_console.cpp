#include "management_console.h"

#include <stdlib.h>
#include <time.h>

#include "as11_rpc.h"
#include "board.h"
#include "debug_log.h"
#include "management_console_format.h"
#include "management_console_utils.h"

namespace aircannect {

namespace {

const char *sensor_state_text(OximetrySensorState state) {
    switch (state) {
        case OximetrySensorState::Off: return "off";
        case OximetrySensorState::Idle: return "idle";
        case OximetrySensorState::Scanning: return "scanning";
        case OximetrySensorState::Connecting: return "connecting";
        case OximetrySensorState::Connected: return "connected";
        case OximetrySensorState::Streaming: return "streaming";
        default: return "unknown";
    }
}

const char *oximetry_source_text(OximetrySource source) {
    switch (source) {
        case OximetrySource::None: return "none";
        case OximetrySource::Udp: return "udp";
        case OximetrySource::Ble: return "ble";
        default: return "unknown";
    }
}

void print_yes_no(Print &out, bool value) {
    out.print(value ? "yes" : "no");
}

void print_app_config_redacted(Print &out, const AppConfigData &cfg) {
    out.println("[CONFIG]");
    out.print("  schema: ");
    out.println(cfg.schema_version);
    out.print("  hostname: ");
    out.println(cfg.hostname);
    out.print("  tcp: ");
    out.print(on_off_text(cfg.tcp_bridge_enabled));
    out.print(" port=");
    out.println(cfg.tcp_bridge_port);
    out.print("  softap: ");
    out.println(softap_mode_name(cfg.softap_mode));
    out.print("  wifi_country: ");
    out.println(cfg.wifi_country);
    out.print("  timezone: ");
    out.println(cfg.timezone);
    out.print("  resmed_time_sync: ");
    out.println(on_off_text(cfg.resmed_time_sync_enabled));
    out.print("  oximetry: ");
    out.print(on_off_text(cfg.oximetry_enabled));
    out.print(" udp_port=");
    out.print(cfg.oximetry_udp_port);
    out.print(" advertise=");
    out.println(oximetry_advertise_mode_name(
        cfg.oximetry_advertise_mode));
    out.print("  http_auth: ");
    out.println(cfg.http_user.length() || cfg.http_password.length()
                    ? "protected"
                    : "open");
    out.print("  http_user: ");
    out.println(cfg.http_user.length() ? cfg.http_user : "<empty>");
    out.print("  http_password: ");
    out.println(cfg.http_password.length() ? "<set>" : "<empty>");
    out.print("  http_whitelist: ");
    out.println(cfg.auth_whitelist.length() ? cfg.auth_whitelist : "<empty>");
    out.print("  telnet: ");
    out.print(on_off_text(cfg.telnet_console_enabled));
    out.print(" port=");
    out.println(cfg.telnet_console_port);
    out.print("  ota: ");
    out.println(cfg.ota_password.length() ? "protected" : "open");
    out.print("  ota_password: ");
    out.println(cfg.ota_password.length() ? "<set>" : "<empty>");
}

void print_wifi_scan(Print &out, WifiManager &wifi_manager) {
    switch (wifi_manager.manual_scan_status()) {
        case WifiScanStatus::RoamInProgress:
            out.println("[WiFi] roaming scan in progress; try again shortly");
            return;
        case WifiScanStatus::Running:
            out.println("[WiFi] scan running; run wifi scan again for results");
            return;
        case WifiScanStatus::Failed:
            out.println("[WiFi] scan failed");
            wifi_manager.clear_manual_scan_results();
            return;
        case WifiScanStatus::Ready: {
            static constexpr size_t MAX_RESULTS = 32;
            WifiScanNetwork results[MAX_RESULTS];
            const size_t count =
                wifi_manager.copy_manual_scan_results(results, MAX_RESULTS);
            for (size_t i = 0; i < count; ++i) {
                out.print("[WiFi] ");
                out.print(i + 1);
                out.print(": ssid=\"");
                out.print(results[i].ssid);
                out.print("\" rssi=");
                out.print(results[i].rssi);
                out.print(" auth=");
                out.println(results[i].open ? "open" : "secured");
            }
            if (!count) out.println("[WiFi] no networks found");
            wifi_manager.clear_manual_scan_results();
            return;
        }
        case WifiScanStatus::Idle:
            break;
    }

    switch (wifi_manager.start_manual_scan()) {
        case WifiScanStartResult::Started:
            out.println("[WiFi] scan started; run wifi scan again for results");
            return;
        case WifiScanStartResult::RoamInProgress:
            out.println("[WiFi] roaming scan in progress; try again shortly");
            return;
        case WifiScanStartResult::Running:
            out.println("[WiFi] scan already running; try again shortly");
            return;
        case WifiScanStartResult::Failed:
            out.println("[WiFi] scan start failed");
            return;
    }
}

void print_oximetry_sensor_status(Print &out,
                                  const OximetryManager &oximetry_manager) {
    const OximetrySensorStatus s = oximetry_manager.sensor_status();
    out.print("[OXI sensor] state=");
    out.print(sensor_state_text(s.sensor_state));
    out.print(" task=");
    print_yes_no(out, s.sensor_task_started);
    out.print(" scanning=");
    print_yes_no(out, s.sensor_scanning);
    out.print(" connected=");
    print_yes_no(out, s.sensor_connected);
    if (s.sensor_peer[0]) {
        out.print(" peer=");
        out.print(s.sensor_peer);
    }
    if (s.sensor_name[0]) {
        out.print(" name=\"");
        out.print(s.sensor_name);
        out.print("\"");
    }
    out.print(" known=");
    out.print(s.sensor_known_count);
    out.print(" results=");
    out.print(s.sensor_scan_count);
    out.print(" notifications=");
    out.print(s.sensor_notifications);
    out.print(" invalid=");
    out.print(s.sensor_invalid_notifications);
    out.print(" connects=");
    out.print(s.sensor_connects);
    out.print(" disconnects=");
    out.print(s.sensor_disconnects);
    out.print(" failures=");
    out.println(s.sensor_connect_failures);
}

void print_oximetry_sensor_scan_results(
    Print &out,
    const OximetryManager &oximetry_manager) {
    out.println("[OXI sensor scan]");
    OximetrySensorDevice snapshot[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    const size_t count =
        oximetry_manager.sensor_scan_results(
            snapshot, AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS);
    for (size_t i = 0; i < count; ++i) {
        out.print("  ");
        out.print(i);
        out.print(": ");
        out.print(snapshot[i].addr);
        out.print(" rssi=");
        out.print(snapshot[i].rssi);
        out.print(" name=\"");
        out.print(snapshot[i].name);
        out.println("\"");
    }
    if (!count) out.println("  <none>");
}

void print_oximetry_sensor_known(Print &out,
                                 const OximetryManager &oximetry_manager) {
    out.println("[OXI sensor known]");
    OximetrySensorDevice snapshot[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    const size_t count =
        oximetry_manager.known_sensors(snapshot,
                                       AC_OXIMETRY_SENSOR_MAX_KNOWN);
    for (size_t i = 0; i < count; ++i) {
        out.print("  ");
        out.print(snapshot[i].addr);
        out.print(" autoconnect=");
        print_yes_no(out, snapshot[i].autoconnect);
        out.print(" name=\"");
        out.print(snapshot[i].name);
        out.println("\"");
    }
    if (!count) out.println("  <none>");
}

}  // namespace

void ManagementConsole::begin(Print &out) {
    out.println("[CLI] ready. Type 'help'.");
}

void ManagementConsole::print_oximetry_status(
    Print &out,
    const OximetryManager &oximetry_manager) const {
    const OximetryRuntimeStatus s = oximetry_manager.runtime_status();
    const OximetrySensorStatus sensor = oximetry_manager.sensor_status();
    out.print("[OXI] enabled=");
    print_yes_no(out, s.enabled);
    out.print(" source=");
    out.print(oximetry_source_text(s.source));
    if (s.source_detail[0]) {
        out.print(":");
        out.print(s.source_detail);
    }
    out.print(" present=");
    print_yes_no(out, s.source_present);
    out.print(" fresh=");
    print_yes_no(out, s.source_fresh);
    out.print(" valid=");
    print_yes_no(out, s.reading.valid);
    out.print(" spo2=");
    if (s.reading.valid) out.print(s.reading.spo2);
    else out.print("--");
    out.print(" pulse=");
    if (s.reading.valid) out.print(s.reading.pulse_bpm);
    else out.print("--");
    out.print(" age_ms=");
    out.print(s.last_source_age_ms);
    out.print(" udp=");
    out.print(s.udp_started ? "listening" : "stopped");
    out.print(":");
    out.print(s.udp_port);
    out.print(" packets=");
    out.print(s.udp_packets);
    out.print("/");
    out.print(s.udp_bad_packets);
    out.print(" advertise=");
    out.print(oximetry_advertise_mode_name(s.advertise_mode));
    out.print(" pair=");
    if (s.pairing_active) {
        out.print("active/");
        out.print((s.pairing_left_ms + 999) / 1000);
        out.print("s");
    } else {
        out.print("off");
    }
    out.print(" ble=");
    out.print(s.ble_available ? "available" : "disabled");
    out.print(" adv=");
    print_yes_no(out, s.advertising);
    out.print(" connected=");
    print_yes_no(out, s.connected);
    out.print(" subscribed=");
    print_yes_no(out, s.subscribed);
    out.print(" disconnect_reason=");
    out.print(s.ble_last_disconnect_reason);
    out.print(" sensor=");
    out.print(sensor_state_text(sensor.sensor_state));
    out.print(" known=");
    out.print(sensor.sensor_known_count);
    out.print(" scan=");
    out.print(sensor.sensor_scan_count);
    if (sensor.sensor_peer[0]) {
        out.print(" peer=");
        out.print(sensor.sensor_peer);
    }
    out.print(" name=\"");
    out.print(s.ble_name);
    out.println("\"");
}

bool ManagementConsole::event_has_output(const RpcEvent &event) {
    switch (event.kind) {
        case RpcEventKind::RpcResponse:
            return event.source == RpcSource::Console ||
                   event.source == RpcSource::Internal;
        case RpcEventKind::RpcUnmatched:
        case RpcEventKind::BootNotification:
        case RpcEventKind::FramingError:
        case RpcEventKind::Info:
            return true;
        case RpcEventKind::RpcNotification:
        case RpcEventKind::DebugLog:
        case RpcEventKind::InternalSettingsStateInvalidated:
        case RpcEventKind::InternalSettingsStateUpdated:
            return false;
    }
    return false;
}

void ManagementConsole::stop(RpcArbiter &arbiter) {
    if (arbiter.stream_consumer_active(stream_handle_)) {
        arbiter.release_stream(stream_handle_);
    }
    stream_handle_ = STREAM_CONSUMER_INVALID;
    line_ = "";
}

void ManagementConsole::poll(Stream &input, Print &out, ConsoleContext &ctx) {
    while (input.available()) {
        char c = static_cast<char>(input.read());
        if (c == '\r' || c == '\n') {
            if (line_.length()) {
                execute_line(line_, out, ctx);
                line_ = "";
            }
        } else if (c == '\b' || c == 0x7F) {
            if (line_.length()) line_.remove(line_.length() - 1);
        } else if (line_.length() < 1024) {
            line_ += c;
        }
    }
}

void ManagementConsole::handle_event(Print &out, const RpcEvent &event) {
    if (!event_has_output(event)) return;

    switch (event.kind) {
        case RpcEventKind::RpcResponse:
            out.print("[RPC response] ");
            out.println(event.payload.c_str());
            break;
        case RpcEventKind::RpcNotification:
            break;
        case RpcEventKind::RpcUnmatched:
            out.print("[RPC unmatched] ");
            out.println(event.payload.c_str());
            break;
        case RpcEventKind::DebugLog:
            break;
        case RpcEventKind::InternalSettingsStateInvalidated:
            break;
        case RpcEventKind::InternalSettingsStateUpdated:
            break;
        case RpcEventKind::BootNotification:
            out.print("[CAN] ");
            out.println(event.payload.c_str());
            break;
        case RpcEventKind::FramingError:
            out.print("[FRAMING] ");
            out.println(event.payload.c_str());
            break;
        case RpcEventKind::Info:
            out.print("[INFO] ");
            out.println(event.payload.c_str());
            break;
    }
}

void ManagementConsole::handle_stream(Print &out, String rest,
                                      RpcArbiter &arbiter) {
    rest.trim();

    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_stream_status(out, arbiter);
        return;
    }

    if (rest == "stop") {
        if (arbiter.stream_consumer_active(stream_handle_)) {
            arbiter.release_stream(stream_handle_);
            stream_handle_ = STREAM_CONSUMER_INVALID;
            out.println("[STREAM] released console subscription");
        } else {
            out.println("[STREAM] no console subscription active");
        }
        ConsoleFormat::print_stream_status(out, arbiter);
        return;
    }

    if (rest.startsWith("{")) {
        StreamAcquireResult result;
        if (arbiter.stream_consumer_active(stream_handle_)) {
            result = arbiter.update_stream(stream_handle_, to_std(rest));
        } else {
            result = arbiter.acquire_stream(to_std(rest), RpcSource::Console);
        }
        if (result.handle >= 0) stream_handle_ = result.handle;
        if (result.status == StreamAcquireStatus::Incompatible) {
            out.println("[STREAM] params conflict with another consumer");
        } else if (result.status == StreamAcquireStatus::Full) {
            out.println("[STREAM] consumer table full");
        } else if (result.status == StreamAcquireStatus::Busy) {
            out.println("[STREAM] control request pending");
        } else if (result.status == StreamAcquireStatus::Rejected) {
            out.println("[STREAM] request rejected");
        } else {
            out.println("[STREAM] console subscription active");
        }
        ConsoleFormat::print_stream_status(out, arbiter);
        return;
    }

    std::string ids = DEFAULT_EDF_STREAM_IDS;
    uint32_t sample_ms = 10;
    uint32_t report_ms = 50;

    if (rest == "fast" || rest == "pressure") {
        ids = "PatientFlow-100hz,Leak-50hz,RespiratoryRate-50hz";
        sample_ms = 10;
        report_ms = 50;
    } else if (rest == "sample") {
        ids = "Leak-50hz,RespiratoryRate-50hz";
        sample_ms = 200;
        report_ms = 1000;
    } else if (rest == "edf" || rest == "full" || rest == "default") {
        ids = DEFAULT_EDF_STREAM_IDS;
        sample_ms = 10;
        report_ms = 50;
    } else if (rest.length()) {
        int split = rest.indexOf(' ');
        ids = to_std(split < 0 ? rest : rest.substring(0, split));
        String tail = split < 0 ? "" : rest.substring(split + 1);
        tail.trim();
        if (tail.length()) {
            int split2 = tail.indexOf(' ');
            String sample = split2 < 0 ? tail : tail.substring(0, split2);
            sample_ms = strtoul(sample.c_str(), nullptr, 0);
            if (sample_ms == 0) sample_ms = 200;
            if (split2 >= 0) {
                String report = tail.substring(split2 + 1);
                report.trim();
                report_ms = report.length() ? strtoul(report.c_str(), nullptr, 0)
                                            : sample_ms * 5;
            } else {
                report_ms = sample_ms * 5;
            }
        }
    }

    std::string params = build_stream_params(ids, sample_ms, report_ms);
    StreamAcquireResult result;
    if (arbiter.stream_consumer_active(stream_handle_)) {
        result = arbiter.update_stream(stream_handle_, params);
    } else {
        result = arbiter.acquire_stream(params, RpcSource::Console);
    }
    if (result.handle >= 0) stream_handle_ = result.handle;

    switch (result.status) {
        case StreamAcquireStatus::Acquired:
            out.println("[STREAM] StartStream queued");
            break;
        case StreamAcquireStatus::AlreadyActive:
            out.println("[STREAM] console subscription active");
            break;
        case StreamAcquireStatus::Incompatible:
            out.println("[STREAM] params conflict with another consumer");
            break;
        case StreamAcquireStatus::Full:
            out.println("[STREAM] consumer table full");
            break;
        case StreamAcquireStatus::Busy:
            out.println("[STREAM] control request pending");
            break;
        case StreamAcquireStatus::Rejected:
        default:
            out.println("[STREAM] request rejected");
            break;
    }
    ConsoleFormat::print_stream_status(out, arbiter);
}

void ManagementConsole::handle_as11(Print &out, String rest,
                                    RpcArbiter &arbiter) {
    rest.trim();
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_as11_status(out, arbiter.as11_state());
        return;
    }

    if (rest == "poll" || rest == "refresh") {
        if (arbiter.request_as11_healthcheck()) {
            out.println("[AS11] healthcheck queued");
        } else {
            out.println("[AS11] healthcheck queue full");
        }
        return;
    }

    if (rest == "version") {
        arbiter.send_request("GetVersion", "", RpcSource::Console);
        return;
    }

    print_unknown_command(out, "AS11", "as11 status, poll, version");
}

void ManagementConsole::handle_therapy(Print &out, String rest,
                                       RpcArbiter &arbiter) {
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_as11_status(out, arbiter.as11_state());
        return;
    }

    if (rest == "start" || rest == "on" || rest == "run") {
        if (arbiter.send_request("EnterTherapy", "", RpcSource::Console)) {
            out.println("[THERAPY] EnterTherapy queued");
        } else {
            out.println("[THERAPY] EnterTherapy queue failed");
        }
        return;
    }

    if (rest == "stop" || rest == "off" || rest == "standby") {
        if (arbiter.send_request("EnterStandby", "", RpcSource::Console)) {
            out.println("[THERAPY] EnterStandby queued");
        } else {
            out.println("[THERAPY] EnterStandby queue failed");
        }
        return;
    }

    print_unknown_command(out, "THERAPY", "therapy status, start, stop");
}

void ManagementConsole::handle_time(Print &out, String rest,
                                    RpcArbiter &arbiter,
                                    TimeSyncService &time_sync_service) {
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        time_t now = time(nullptr);
        struct tm utc = {};
        struct tm local = {};
        gmtime_r(&now, &utc);
        localtime_r(&now, &local);
        char utc_text[24];
        char local_text[24];
        strftime(utc_text, sizeof(utc_text), "%Y-%m-%d %H:%M:%S", &utc);
        strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S",
                 &local);
        out.print("[TIME] utc=");
        out.print(time_sync_service.esp_clock_valid() ? utc_text : "invalid");
        out.print(" local=");
        out.print(time_sync_service.esp_clock_valid() ? local_text : "invalid");
        out.print(" epoch=");
        out.print(static_cast<uint32_t>(now));
        out.print(" source=");
        out.print(time_sync_service.esp_clock_source_name());
        out.print(" ntp=");
        out.print(time_sync_service.ntp_synced() ? "synced" : "not_synced");
        out.print(" resmed_push=");
        out.print(time_sync_service.resmed_time_sync_enabled() ? "on" : "off");
        out.print(" resmed_offset_ms=");
        const As11DeviceState &as11 = arbiter.as11_state();
        if (as11.clock_offset_valid()) {
            out.print(as11.clock_offset_ms());
        } else {
            out.print("unknown");
        }
        out.print(" status=");
        out.println(time_sync_service.last_status());
        return;
    }

    if (rest == "get") {
        arbiter.send_request("GetDateTime", "", RpcSource::Console);
        return;
    }

    if (rest == "set" || rest == "push" || rest == "sync-to-resmed") {
        if (time_sync_service.request_push_esp_to_resmed(RpcSource::Console)) {
            out.println("[TIME] SetDateTime queued");
        } else {
            out.println("[TIME] ESP clock is not ready or queue is full");
        }
        return;
    }

    if (rest == "pull" || rest == "sync-from-resmed") {
        if (time_sync_service.request_pull_resmed_to_esp(RpcSource::Console)) {
            out.println("[TIME] GetDateTime queued for ESP clock sync");
        } else {
            out.println("[TIME] GetDateTime queue failed");
        }
        return;
    }

    if (rest == "ntp") {
        time_sync_service.force_ntp_sync();
        out.println("[TIME] NTP resync triggered");
        return;
    }

    print_unknown_command(out, "TIME", "time, get, push, pull, ntp");
}

void ManagementConsole::handle_ota(Print &out, String rest,
                                   OtaManager &ota_manager) {
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        const OtaManagerStatus ota = ota_manager.status();
        out.print("[OTA] arduino=");
        out.print(ota.arduino_started ? "started" : "stopped");
        out.print(" port=");
        out.print(ota.arduino_port);
        out.print(" auth=");
        out.print(ota.auth_enabled ? "on" : "off");
        out.print(" http=");
        out.print(ota.http_active ? "active" : (ota.http_ready ? "ready" : "idle"));
        out.print(" method=");
        out.print(ota.method);
        out.print(" bytes=");
        out.print(static_cast<unsigned long>(ota.bytes));
        out.print(" progress=");
        out.print(ota.progress_percent);
        out.print("% partition=");
        out.print(ota.partition.length() ? ota.partition : "--");
        if (ota.reboot_pending) out.print(" restart=pending");
        if (ota.last_error.length()) {
            out.print(" error=");
            out.print(ota.last_error);
        }
        out.println();
        return;
    }

    print_unknown_command(out, "OTA", "ota status");
}

void ManagementConsole::handle_resmed_ota(
    Print &out,
    String rest,
    ResmedOtaManager &resmed_ota_manager) {
    rest.trim();
    if (!rest.length() || rest == "status") {
        const ResmedOtaStatus status = resmed_ota_manager.status();
        out.print("[RESMED OTA] phase=");
        out.print(resmed_ota_manager.phase_name());
        out.print(" waiting=");
        out.print(status.waiting ? "yes" : "no");
        out.print(" file=\"");
        out.print(status.filename);
        out.print("\" total=");
        out.print(static_cast<unsigned long>(status.total_size));
        out.print(" uploaded=");
        out.print(static_cast<unsigned long>(status.uploaded_bytes));
        out.print(" progress=");
        out.print(status.progress_percent);
        out.print("% block=");
        out.print(static_cast<unsigned long>(status.xfer_block_size));
        if (status.computed_sha256.length()) {
            out.print(" sha256=");
            out.print(status.computed_sha256);
        }
        if (status.apply_mode.length()) {
            out.print(" apply=");
            out.print(status.apply_mode);
        }
        if (status.last_error.length()) {
            out.print(" error=");
            out.print(status.last_error);
        }
        out.println();
        return;
    }

    if (rest == "check") {
        if (resmed_ota_manager.request_check()) {
            out.println("[RESMED OTA] CheckUpgradeFile queued");
        } else {
            const ResmedOtaStatus status = resmed_ota_manager.status();
            out.print("[RESMED OTA] check failed: ");
            out.println(status.last_error);
        }
        return;
    }

    if (rest == "abort") {
        resmed_ota_manager.abort("aborted_by_console");
        out.println("[RESMED OTA] aborted");
        return;
    }

    if (rest.startsWith("apply ")) {
        String args = rest.substring(6);
        int pos = 0;
        String mode;
        if (!parse_console_arg(args, pos, mode)) {
            out.println("[RESMED OTA] usage: resmed-ota apply plain CONFIRM");
            return;
        }
        mode.toLowerCase();
        if (mode == "plain") {
            String confirm;
            if (!parse_console_arg(args, pos, confirm)) {
                out.print("[RESMED OTA] confirmation required: ");
                out.println(AC_RESMED_OTA_CONFIRM);
                return;
            }
            if (resmed_ota_manager.request_apply_plain(false, confirm)) {
                out.println("[RESMED OTA] ApplyUpgrade queued");
            } else {
                const ResmedOtaStatus status = resmed_ota_manager.status();
                out.print("[RESMED OTA] apply failed: ");
                out.println(status.last_error);
            }
            return;
        }
        if (mode == "authenticated") {
            String tag;
            String confirm;
            if (!parse_console_arg(args, pos, tag) ||
                !parse_console_arg(args, pos, confirm)) {
                out.println("[RESMED OTA] usage: resmed-ota apply authenticated TAG CONFIRM");
                return;
            }
            if (resmed_ota_manager.request_apply_authenticated(tag, confirm)) {
                out.println("[RESMED OTA] ApplyAuthenticatedUpgrade queued");
            } else {
                const ResmedOtaStatus status = resmed_ota_manager.status();
                out.print("[RESMED OTA] apply failed: ");
                out.println(status.last_error);
            }
            return;
        }
    }

    print_unknown_command(out, "RESMED OTA", "status, check, abort, apply");
}

void ManagementConsole::handle_sink(Print &out,
                                    String rest,
                                    SinkManager &sink_manager) {
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_sink_status(out, sink_manager);
        return;
    }

    if (rest == "debug on" || rest == "debug enable" ||
        rest == "debug enabled") {
        sink_manager.set_debug_enabled(true);
        out.println("[SINK] debug sink enabled");
        ConsoleFormat::print_sink_status(out, sink_manager);
        return;
    }

    if (rest == "debug off" || rest == "debug disable" ||
        rest == "debug disabled") {
        sink_manager.set_debug_enabled(false);
        out.println("[SINK] debug sink disabled");
        ConsoleFormat::print_sink_status(out, sink_manager);
        return;
    }

    print_unknown_command(out, "SINK", "sink status, sink debug on|off");
}

void ManagementConsole::handle_oximetry(
    Print &out,
    String rest,
    OximetryManager &oximetry_manager) {
    rest.trim();
    String lower = rest;
    lower.toLowerCase();
    if (!lower.length() || lower == "status") {
        print_oximetry_status(out, oximetry_manager);
        return;
    }

    if (lower == "on" || lower == "enable" || lower == "enabled") {
        if (!oximetry_manager.set_enabled(true)) {
            out.println("[OXI] failed to enable");
            return;
        }
        out.println("[OXI] enabled");
        print_oximetry_status(out, oximetry_manager);
        return;
    }

    if (lower == "off" || lower == "disable" || lower == "disabled") {
        if (!oximetry_manager.set_enabled(false)) {
            out.println("[OXI] failed to disable");
            return;
        }
        out.println("[OXI] disabled");
        print_oximetry_status(out, oximetry_manager);
        return;
    }

    if (lower == "cpap pair" || lower == "cpap pairing" ||
        lower == "cpap pair start" || lower == "cpap pairing start") {
        if (!oximetry_manager.set_enabled(true)) {
            out.println("[OXI] failed to enable");
            return;
        }
        oximetry_manager.request_pairing(true);
        out.println("[OXI] CPAP pairing window started");
        print_oximetry_status(out, oximetry_manager);
        return;
    }

    if (lower == "cpap pair stop" || lower == "cpap pairing stop") {
        oximetry_manager.request_pairing(false);
        out.println("[OXI] CPAP pairing window stopped");
        print_oximetry_status(out, oximetry_manager);
        return;
    }

    if (lower == "cpap forget" || lower == "cpap forget-bonds") {
        if (oximetry_manager.forget_bonds()) {
            out.println("[OXI] CPAP BLE bonds cleared");
        } else {
            out.println("[OXI] CPAP BLE bond clear failed");
        }
        return;
    }

    if (lower == "cpap status") {
        print_oximetry_status(out, oximetry_manager);
        return;
    }

    if (lower == "sensor" || lower == "sensor status") {
        print_oximetry_sensor_status(out, oximetry_manager);
        return;
    }

    if (lower == "sensor scan") {
        if (oximetry_manager.request_sensor_scan()) {
            out.println("[OXI sensor] scan queued");
        } else {
            out.println("[OXI sensor] scan failed");
        }
        print_oximetry_sensor_status(out, oximetry_manager);
        return;
    }

    if (lower == "sensor results" || lower == "sensor scan-results") {
        print_oximetry_sensor_scan_results(out, oximetry_manager);
        return;
    }

    if (lower == "sensor list" || lower == "sensor known") {
        print_oximetry_sensor_known(out, oximetry_manager);
        return;
    }

    if (lower == "sensor disconnect") {
        oximetry_manager.request_sensor_disconnect();
        out.println("[OXI sensor] disconnect queued");
        print_oximetry_sensor_status(out, oximetry_manager);
        return;
    }

    if (lower.startsWith("sensor connect ")) {
        String target = rest.substring(15);
        target.trim();
        if (!oximetry_manager.request_sensor_connect(target.c_str())) {
            out.println("[OXI sensor] connect failed; use scan result index or known address");
            return;
        }
        out.println("[OXI sensor] connect queued");
        print_oximetry_sensor_status(out, oximetry_manager);
        return;
    }

    if (lower.startsWith("sensor forget ")) {
        String target = rest.substring(14);
        target.trim();
        if (!oximetry_manager.forget_sensor(target.c_str())) {
            out.println("[OXI sensor] forget failed");
            return;
        }
        out.println("[OXI sensor] forgotten");
        print_oximetry_sensor_known(out, oximetry_manager);
        return;
    }

    if (lower.startsWith("sensor autoconnect ")) {
        String args = rest.substring(19);
        args.trim();
        const int split = args.lastIndexOf(' ');
        if (split <= 0) {
            out.println("[OXI sensor] usage: oxi sensor autoconnect ADDR on|off");
            return;
        }
        String addr = args.substring(0, split);
        String value = args.substring(split + 1);
        bool enabled = false;
        if (!parse_on_off(value, enabled) ||
            !oximetry_manager.set_sensor_autoconnect(addr.c_str(),
                                                     enabled)) {
            out.println("[OXI sensor] autoconnect failed");
            return;
        }
        out.print("[OXI sensor] autoconnect=");
        out.println(on_off_text(enabled));
        print_oximetry_sensor_known(out, oximetry_manager);
        return;
    }

    if (lower.startsWith("advertise ")) {
        String mode = lower.substring(10);
        mode.trim();
        if (mode == "start" || mode == "on") {
            oximetry_manager.request_advertising(true);
            out.println("[OXI] manual advertising requested");
            print_oximetry_status(out, oximetry_manager);
            return;
        }
        if (mode == "stop" || mode == "off") {
            oximetry_manager.request_advertising(false);
            out.println("[OXI] manual advertising stopped");
            print_oximetry_status(out, oximetry_manager);
            return;
        }

        OximetryAdvertiseMode adv_mode;
        if (!parse_oximetry_advertise_mode(mode, adv_mode) ||
            !oximetry_manager.set_advertise_mode(adv_mode)) {
            out.println("[OXI] usage: oxi advertise auto|manual|start|stop");
            return;
        }
        out.print("[OXI] advertise=");
        out.println(oximetry_advertise_mode_name(adv_mode));
        return;
    }

    if (lower == "forget" || lower == "forget-bonds") {
        out.println("[OXI] use: oxi cpap forget");
        return;
    }

    print_unknown_command(out, "OXI",
                          "oxi status, on, off, cpap, sensor, advertise");
}

void ManagementConsole::handle_log(Print &out,
                                   String rest,
                                   AppConfig &app_config) {
    rest.trim();
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (rest.startsWith("level ")) {
        String args = rest.substring(6);
        int pos = 0;
        String first;
        if (!parse_console_arg(args, pos, first)) {
            out.println("[LOG] usage: log level LEVEL | log level CATEGORY LEVEL");
            return;
        }

        log_level_t level = LOG_INFO;
        log_cat_t cat = CAT_GENERAL;
        if (Log::parse_level(first, level)) {
            if (!app_config.set_all_log_levels(level)) {
                out.println("[LOG] failed to store level");
                return;
            }
            Log::set_level(level);
            ConsoleFormat::print_log_status(out);
            return;
        }

        if (!Log::parse_cat(first, cat)) {
            out.println("[LOG] unknown category or level");
            return;
        }
        String level_text;
        if (!parse_console_arg(args, pos, level_text) ||
            !Log::parse_level(level_text, level)) {
            out.println("[LOG] usage: log level CATEGORY LEVEL");
            return;
        }
        if (!app_config.set_log_level(cat, level)) {
            out.println("[LOG] failed to store level");
            return;
        }
        Log::set_cat_level(cat, level);
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (rest.startsWith("syslog")) {
        String args = rest.length() > 6 ? rest.substring(6) : "";
        args.trim();
        if (!args.length() || args == "status") {
            ConsoleFormat::print_log_status(out);
            return;
        }

        int pos = 0;
        String host;
        if (!parse_console_arg(args, pos, host)) {
            out.println("[LOG] usage: log syslog off|HOST [PORT]");
            return;
        }
        String host_lower = host;
        host_lower.toLowerCase();
        if (host_lower == "off" || host_lower == "disable" ||
            host_lower == "disabled" || host_lower == "0") {
            if (!app_config.set_syslog(false, "",
                                       app_config.data().syslog_port)) {
                out.println("[LOG] failed to store syslog config");
                return;
            }
            app_config.apply_log_config();
            ConsoleFormat::print_log_status(out);
            return;
        }

        uint16_t port = app_config.data().syslog_port;
        String port_text;
        if (parse_console_arg(args, pos, port_text)) {
            if (!parse_uint16_arg(port_text, port)) {
                out.println("[LOG] invalid syslog port");
                return;
            }
        }
        if (!app_config.set_syslog(true, host, port)) {
            out.println("[LOG] syslog host must be an IPv4 address");
            return;
        }
        app_config.apply_log_config();
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (rest == "test" || rest.startsWith("test ")) {
        String text = rest.length() > 4 ? rest.substring(5) : "test";
        text.trim();
        if (!text.length()) text = "test";
        Log::logf(CAT_CLI, LOG_INFO, "[LOG] %s\n", text.c_str());
        out.println("[LOG] test emitted");
        return;
    }

    print_unknown_command(out, "LOG", "log status, level, syslog, test");
}

void ManagementConsole::handle_wifi(Print &out, String rest,
                                    WifiManager &wifi_manager,
                                    TcpBridge &tcp_bridge,
                                    const AppConfig &app_config) {
    rest.trim();
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest == "list") {
        out.print("[WiFi profiles] count=");
        out.print(wifi_manager.profile_count());
        out.print(" active=");
        const int8_t active = wifi_manager.active_profile_index();
        if (active >= 0) {
            out.println(active);
        } else {
            out.println("none");
        }
        for (size_t i = 0; i < wifi_manager.profile_count(); ++i) {
            const WifiProfile &profile = wifi_manager.profile(i);
            out.print("  ");
            out.print(i);
            out.print(": ssid=\"");
            out.print(profile.ssid);
            out.print("\" auth=");
            out.print(profile.password.length() == 0 ? "open" : "password");
            if (active == static_cast<int8_t>(i)) {
                out.print(" active=yes");
            }
            out.println();
        }
        return;
    }

    if (rest == "scan") {
        print_wifi_scan(out, wifi_manager);
        return;
    }

    if (rest == "restart" || rest == "reconnect") {
        out.println("[WiFi] reconnecting...");
        bool ok = wifi_manager.reconnect();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        if (!ok) {
            out.println("[WiFi] no STA credentials and SoftAP fallback is off");
        } else if (!wifi_manager.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest == "clear") {
        out.println("[WiFi] clearing stored STA credentials");
        wifi_manager.clear_sta_config();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest.startsWith("set ")) {
        String args = rest.substring(4);
        int pos = 0;
        String ssid;
        String password;
        if (!parse_console_arg(args, pos, ssid) ||
            !parse_console_arg(args, pos, password) ||
            !ssid.length()) {
            out.println("[WiFi] usage: wifi set SSID PASSWORD");
            out.println("[WiFi] quote SSID/password when they contain spaces");
            return;
        }

        out.print("[WiFi] saving STA credentials for SSID=\"");
        out.print(ssid);
        out.println("\"");
        bool ok = wifi_manager.configure_sta(ssid, password);
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        if (!ok) {
            out.println("[WiFi] STA connect could not be started");
        } else if (!wifi_manager.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest.startsWith("add ")) {
        String args = rest.substring(4);
        int pos = 0;
        String ssid;
        String password;
        if (!parse_console_arg(args, pos, ssid) ||
            !parse_console_arg(args, pos, password) ||
            !ssid.length()) {
            out.println("[WiFi] usage: wifi add SSID PASSWORD");
            out.println("[WiFi] quote SSID/password when they contain spaces");
            return;
        }

        out.print("[WiFi] adding STA profile SSID=\"");
        out.print(ssid);
        out.println("\"");
        bool ok = wifi_manager.add_profile(ssid, password, false);
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        if (!ok) out.println("[WiFi] profile add failed");
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest.startsWith("open ")) {
        String args = rest.substring(5);
        int pos = 0;
        String ssid;
        if (!parse_console_arg(args, pos, ssid) || !ssid.length()) {
            out.println("[WiFi] usage: wifi open SSID");
            return;
        }

        out.print("[WiFi] saving open STA network SSID=\"");
        out.print(ssid);
        out.println("\"");
        bool ok = wifi_manager.configure_open_sta(ssid);
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        if (!ok) {
            out.println("[WiFi] STA connect could not be started");
        } else if (!wifi_manager.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest.startsWith("remove ")) {
        String index_text = rest.substring(7);
        size_t index = 0;
        if (!parse_index_arg(index_text, wifi_manager.profile_count(), index)) {
            out.println("[WiFi] usage: wifi remove INDEX");
            return;
        }
        if (!wifi_manager.remove_profile(index)) {
            out.println("[WiFi] profile remove failed");
            return;
        }
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    print_unknown_command(out, "WiFi",
                          "wifi status, list, scan, set, add, open, remove, clear, restart");
}

void ManagementConsole::handle_config(Print &out, String rest,
                                      AppConfig &app_config,
                                      WifiManager &wifi_manager,
                                      TcpBridge &tcp_bridge,
                                      OtaManager &ota_manager) {
    rest.trim();
    if (!rest.length() || rest == "show" || rest == "dump") {
        print_app_config_redacted(out, app_config.data());
        return;
    }

    if (rest == "factory-reset" || rest == "factory reset") {
        out.println("[CONFIG] factory reset: clearing app config and Wi-Fi credentials");
        app_config.factory_reset();
        wifi_manager.set_hostname(app_config.data().hostname);
        wifi_manager.set_softap_mode(app_config.data().softap_mode);
        wifi_manager.set_country_code(app_config.data().wifi_country);
        ota_manager.mark_config_dirty();
        wifi_manager.clear_sta_config();
        app_config.apply_log_config();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        out.println("[CONFIG] factory reset complete");
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest == "reset") {
        out.println("[CONFIG] resetting app config to defaults");
        app_config.factory_reset();
        wifi_manager.set_hostname(app_config.data().hostname);
        wifi_manager.set_softap_mode(app_config.data().softap_mode);
        wifi_manager.set_country_code(app_config.data().wifi_country);
        ota_manager.mark_config_dirty();
        wifi_manager.reconnect();
        app_config.apply_log_config();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        out.println("[CONFIG] reset complete");
        return;
    }

    if (rest == "hostname") {
        out.print("[CONFIG] hostname=");
        out.println(app_config.data().hostname);
        return;
    }

    if (rest.startsWith("hostname ")) {
        String hostname = rest.substring(9);
        hostname.trim();
        if (!app_config.set_hostname(hostname)) {
            out.println("[CONFIG] invalid hostname; use 1-63 alnum/hyphen chars, not starting or ending with hyphen");
            return;
        }
        wifi_manager.set_hostname(app_config.data().hostname);
        ota_manager.mark_config_dirty();
        app_config.apply_log_config();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        out.print("[CONFIG] hostname=");
        out.println(app_config.data().hostname);
        return;
    }

    if (rest == "tcp") {
        out.print("[CONFIG] tcp=");
        out.print(on_off_text(app_config.data().tcp_bridge_enabled));
        out.print(" port=");
        out.println(app_config.data().tcp_bridge_port);
        return;
    }

    if (rest.startsWith("tcp ")) {
        String args = rest.substring(4);
        int pos = 0;
        String state;
        if (!parse_console_arg(args, pos, state)) {
            out.println("[CONFIG] usage: config tcp on|off [PORT]");
            return;
        }

        bool enabled = false;
        uint16_t port = app_config.data().tcp_bridge_port;
        if (state == "port") {
            String port_arg;
            if (!parse_console_arg(args, pos, port_arg)) {
                out.println("[CONFIG] usage: config tcp port PORT");
                return;
            }
            if (!parse_uint16_arg(port_arg, port)) {
                out.println("[CONFIG] invalid TCP port");
                return;
            }
            enabled = app_config.data().tcp_bridge_enabled;
        } else {
            if (!parse_on_off(state, enabled)) {
                out.println("[CONFIG] usage: config tcp on|off [PORT]");
                return;
            }
            String port_arg;
            if (parse_console_arg(args, pos, port_arg)) {
                if (!parse_uint16_arg(port_arg, port)) {
                    out.println("[CONFIG] invalid TCP port");
                    return;
                }
            }
        }

        if (!app_config.set_tcp_bridge(enabled, port)) {
            out.println("[CONFIG] failed to store TCP bridge config");
            return;
        }
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        out.print("[CONFIG] tcp=");
        out.print(on_off_text(app_config.data().tcp_bridge_enabled));
        out.print(" port=");
        out.println(app_config.data().tcp_bridge_port);
        return;
    }

    if (rest == "softap") {
        out.print("[CONFIG] softap=");
        out.println(softap_mode_name(app_config.data().softap_mode));
        return;
    }

    if (rest.startsWith("softap ")) {
        String state = rest.substring(7);
        SoftApMode mode;
        if (!parse_softap_mode(state, mode)) {
            out.println("[CONFIG] usage: config softap auto|forced");
            return;
        }
        const bool should_reconnect =
            wifi_manager.mode_state() == WifiModeState::SoftAp &&
            mode == SoftApMode::Auto &&
            wifi_manager.has_sta_config();
        app_config.set_softap_mode(mode);
        wifi_manager.set_softap_mode(app_config.data().softap_mode);
        wifi_manager.apply_softap_mode();
        if (should_reconnect) wifi_manager.reconnect();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        out.print("[CONFIG] softap=");
        out.println(softap_mode_name(app_config.data().softap_mode));
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest == "wifi-country") {
        out.print("[CONFIG] wifi_country=");
        out.println(app_config.data().wifi_country);
        return;
    }

    if (rest.startsWith("wifi-country ")) {
        String country = rest.substring(13);
        country.trim();
        if (!app_config.set_wifi_country(country)) {
            out.println("[CONFIG] invalid Wi-Fi country; use ISO CC, 01, or default");
            return;
        }
        wifi_manager.set_country_code(app_config.data().wifi_country);
        wifi_manager.reconnect();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        out.print("[CONFIG] wifi_country=");
        out.println(app_config.data().wifi_country);
        ConsoleFormat::print_wifi_status(out, wifi_manager);
        return;
    }

    if (rest == "timezone") {
        out.print("[CONFIG] timezone=");
        out.println(app_config.data().timezone);
        return;
    }

    if (rest.startsWith("timezone ")) {
        String timezone = rest.substring(9);
        if (!app_config.set_timezone(timezone)) {
            out.println("[CONFIG] invalid timezone");
            return;
        }
        out.print("[CONFIG] timezone=");
        out.println(app_config.data().timezone);
        return;
    }

    if (rest == "resmed-time-sync") {
        out.print("[CONFIG] resmed_time_sync=");
        out.println(on_off_text(app_config.data().resmed_time_sync_enabled));
        return;
    }

    if (rest.startsWith("resmed-time-sync ")) {
        String state = rest.substring(17);
        bool enabled = false;
        if (!parse_on_off(state, enabled)) {
            out.println("[CONFIG] usage: config resmed-time-sync on|off");
            return;
        }
        if (!app_config.set_resmed_time_sync(enabled)) {
            out.println("[CONFIG] failed to store ResMed time sync setting");
            return;
        }
        out.print("[CONFIG] resmed_time_sync=");
        out.println(on_off_text(app_config.data().resmed_time_sync_enabled));
        return;
    }

    if (rest == "oximetry") {
        out.print("[CONFIG] oximetry=");
        out.print(on_off_text(app_config.data().oximetry_enabled));
        out.print(" udp_port=");
        out.print(app_config.data().oximetry_udp_port);
        out.print(" advertise=");
        out.println(oximetry_advertise_mode_name(
            app_config.data().oximetry_advertise_mode));
        return;
    }

    if (rest.startsWith("oximetry ")) {
        String args = rest.substring(9);
        int pos = 0;
        String key;
        if (!parse_console_arg(args, pos, key)) {
            out.println("[CONFIG] usage: config oximetry on|off|udp-port PORT|advertise auto|manual");
            return;
        }
        key.toLowerCase();
        bool ok = false;
        if (key == "on" || key == "off" || key == "enable" ||
            key == "disable" || key == "enabled" || key == "disabled") {
            bool enabled = false;
            if (key == "enable") enabled = true;
            else if (key == "disable") enabled = false;
            else if (!parse_on_off(key, enabled)) {
                out.println("[CONFIG] usage: config oximetry on|off");
                return;
            }
            ok = app_config.set_oximetry_enabled(enabled);
        } else if (key == "udp-port" || key == "port") {
            String port_arg;
            uint16_t port = 0;
            if (!parse_console_arg(args, pos, port_arg) ||
                !parse_uint16_arg(port_arg, port)) {
                out.println("[CONFIG] usage: config oximetry udp-port PORT");
                return;
            }
            ok = app_config.set_oximetry_udp_port(port);
        } else if (key == "advertise" || key == "adv") {
            String mode_arg;
            OximetryAdvertiseMode mode;
            if (!parse_console_arg(args, pos, mode_arg) ||
                !parse_oximetry_advertise_mode(mode_arg, mode)) {
                out.println("[CONFIG] usage: config oximetry advertise auto|manual");
                return;
            }
            ok = app_config.set_oximetry_advertise_mode(mode);
        } else {
            out.println("[CONFIG] usage: config oximetry on|off|udp-port PORT|advertise auto|manual");
            return;
        }
        if (!ok) {
            out.println("[CONFIG] failed to store oximetry config");
            return;
        }
        out.print("[CONFIG] oximetry=");
        out.print(on_off_text(app_config.data().oximetry_enabled));
        out.print(" udp_port=");
        out.print(app_config.data().oximetry_udp_port);
        out.print(" advertise=");
        out.println(oximetry_advertise_mode_name(
            app_config.data().oximetry_advertise_mode));
        return;
    }

    if (rest == "http-auth") {
        out.print("[CONFIG] http_auth=");
        out.print(app_config.data().http_user.length() ||
                          app_config.data().http_password.length()
                      ? "protected"
                      : "open");
        out.print(" user=");
        out.print(app_config.data().http_user.length()
                      ? app_config.data().http_user.c_str()
                      : "<empty>");
        out.print(" password=");
        out.println(app_config.data().http_password.length() ? "<set>"
                                                             : "<empty>");
        return;
    }

    if (rest.startsWith("http-auth ")) {
        String args = rest.substring(10);
        int pos = 0;
        String user;
        String password;
        if (!parse_console_arg(args, pos, user) ||
            !parse_console_arg(args, pos, password)) {
            out.println("[CONFIG] usage: config http-auth USER PASSWORD");
            out.println("[CONFIG] use: config http-auth \"\" \"\" to allow open HTTP access");
            return;
        }
        if (!app_config.set_http_auth(user, password)) {
            out.println("[CONFIG] invalid HTTP credentials");
            return;
        }
        out.print("[CONFIG] http_auth=");
        out.println(app_config.data().http_user.length() ||
                            app_config.data().http_password.length()
                        ? "protected"
                        : "open");
        return;
    }

    if (rest == "http-whitelist") {
        out.print("[CONFIG] http_whitelist=");
        out.println(app_config.data().auth_whitelist.length()
                        ? app_config.data().auth_whitelist.c_str()
                        : "<empty>");
        return;
    }

    if (rest.startsWith("http-whitelist ")) {
        String whitelist = rest.substring(15);
        whitelist.trim();
        if (!app_config.set_auth_whitelist(whitelist)) {
            out.println("[CONFIG] invalid HTTP whitelist");
            return;
        }
        out.print("[CONFIG] http_whitelist=");
        out.println(app_config.data().auth_whitelist.length()
                        ? app_config.data().auth_whitelist.c_str()
                        : "<empty>");
        return;
    }

    if (rest == "telnet") {
        out.print("[CONFIG] telnet=");
        out.print(on_off_text(app_config.data().telnet_console_enabled));
        out.print(" port=");
        out.println(app_config.data().telnet_console_port);
        return;
    }

    if (rest.startsWith("telnet ")) {
        String args = rest.substring(7);
        int pos = 0;
        String state;
        if (!parse_console_arg(args, pos, state)) {
            out.println("[CONFIG] usage: config telnet on|off [PORT]");
            return;
        }

        bool enabled = false;
        uint16_t port = app_config.data().telnet_console_port;
        if (state == "port") {
            String port_arg;
            if (!parse_console_arg(args, pos, port_arg)) {
                out.println("[CONFIG] usage: config telnet port PORT");
                return;
            }
            if (!parse_uint16_arg(port_arg, port)) {
                out.println("[CONFIG] invalid telnet port");
                return;
            }
            enabled = app_config.data().telnet_console_enabled;
        } else {
            if (!parse_on_off(state, enabled)) {
                out.println("[CONFIG] usage: config telnet on|off [PORT]");
                return;
            }
            String port_arg;
            if (parse_console_arg(args, pos, port_arg)) {
                if (!parse_uint16_arg(port_arg, port)) {
                    out.println("[CONFIG] invalid telnet port");
                    return;
                }
            }
        }

        if (!app_config.set_telnet_console(enabled, port)) {
            out.println("[CONFIG] failed to store telnet config");
            return;
        }
        out.print("[CONFIG] telnet=");
        out.print(on_off_text(app_config.data().telnet_console_enabled));
        out.print(" port=");
        out.println(app_config.data().telnet_console_port);
        return;
    }

    if (rest == "ota-password") {
        out.print("[CONFIG] ota_password=");
        out.println(app_config.data().ota_password.length() ? "<set>"
                                                            : "<empty>");
        return;
    }

    if (rest.startsWith("ota-password ")) {
        String args = rest.substring(13);
        int pos = 0;
        String password;
        if (!parse_console_arg(args, pos, password)) {
            out.println("[CONFIG] usage: config ota-password PASSWORD");
            out.println("[CONFIG] use: config ota-password \"\" to allow open ArduinoOTA access");
            return;
        }
        if (!app_config.set_ota_password(password)) {
            out.println("[CONFIG] invalid OTA password");
            return;
        }
        ota_manager.mark_config_dirty();
        out.print("[CONFIG] ota_password=");
        out.println(app_config.data().ota_password.length() ? "<set>"
                                                            : "<empty>");
        return;
    }

    print_unknown_command(out, "CONFIG",
                          "config, hostname, tcp, softap, wifi-country, timezone, resmed-time-sync, oximetry, http-auth, http-whitelist, telnet, ota-password, reset, factory-reset");
}

void ManagementConsole::apply_runtime_config(const AppConfig &app_config,
                                             WifiManager &wifi_manager,
                                             TcpBridge &tcp_bridge) {
    wifi_manager.set_hostname(app_config.data().hostname);
    wifi_manager.set_softap_mode(app_config.data().softap_mode);
    wifi_manager.set_country_code(app_config.data().wifi_country);
    wifi_manager.apply_softap_mode();

    if (!wifi_manager.network_available() ||
        !app_config.data().tcp_bridge_enabled) {
        tcp_bridge.stop();
        return;
    }

    tcp_bridge.restart(app_config.data().tcp_bridge_port);
}

}  // namespace aircannect
