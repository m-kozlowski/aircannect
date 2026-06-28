#include "management_console.h"

#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#include "as11_rpc.h"
#include "board.h"
#include "debug_log.h"
#include "edf_stream_signal_table.h"
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

const char *secret_state_text(const String &value) {
    return value.length() ? "<set>" : "<empty>";
}

void print_app_config_redacted(Print &out, const AppConfigData &cfg) {
    out.println("[CONFIG]");
    out.print("  host: ");
    out.println(cfg.hostname);
    out.print("  tcp_en: ");
    out.println(on_off_text(cfg.tcp_bridge_enabled));
    out.print("  tcp_port: ");
    out.println(cfg.tcp_bridge_port);
    out.print("  softap_mode: ");
    out.println(softap_mode_name(cfg.softap_mode));
    out.print("  wifi_ctry: ");
    out.println(cfg.wifi_country);
    out.print("  tz: ");
    out.println(cfg.timezone);
    out.print("  resmed_time: ");
    out.println(on_off_text(cfg.resmed_time_sync_enabled));
    out.print("  oxi_en: ");
    out.println(on_off_text(cfg.oximetry_enabled));
    out.print("  oxi_udp: ");
    out.println(cfg.oximetry_udp_port);
    out.print("  oxi_adv: ");
    out.println(oximetry_advertise_mode_name(
        cfg.oximetry_advertise_mode));
    out.print("  edf_cap: ");
    out.println(on_off_text(cfg.edf_capture_enabled));
    out.print("  smb_ep: ");
    out.println(cfg.smb_endpoint.length() ? cfg.smb_endpoint : "<empty>");
    out.print("  smb_user: ");
    out.println(cfg.smb_user.length() ? cfg.smb_user : "<empty>");
    out.print("  smb_pass: ");
    out.println(secret_state_text(cfg.smb_password));
    out.print("  shq_id: ");
    out.println(cfg.sleephq_client_id.length()
                    ? cfg.sleephq_client_id
                    : "<empty>");
    out.print("  shq_secret: ");
    out.println(secret_state_text(cfg.sleephq_client_secret));
    out.print("  shq_team: ");
    out.println(cfg.sleephq_team_id.length()
                    ? cfg.sleephq_team_id
                    : "<empty>");
    out.print("  shq_device: ");
    out.println(cfg.sleephq_device_id.length()
                    ? cfg.sleephq_device_id
                    : "<empty>");
    out.print("  http_user: ");
    out.println(cfg.http_user.length() ? cfg.http_user : "<empty>");
    out.print("  http_pass: ");
    out.println(secret_state_text(cfg.http_password));
    out.print("  auth_wl: ");
    out.println(cfg.auth_whitelist.length() ? cfg.auth_whitelist : "<empty>");
    out.print("  telnet_en: ");
    out.println(on_off_text(cfg.telnet_console_enabled));
    out.print("  telnet_port: ");
    out.println(cfg.telnet_console_port);
    out.print("  ota_pass: ");
    out.println(secret_state_text(cfg.ota_password));
    out.print("  syslog_en: ");
    out.println(on_off_text(cfg.syslog_enabled));
    out.print("  syslog_host: ");
    out.println(cfg.syslog_host.length() ? cfg.syslog_host : "<empty>");
    out.print("  syslog_port: ");
    out.println(cfg.syslog_port);
    out.print("  file_log_en: ");
    out.println(on_off_text(cfg.file_log_enabled));
}

String trim_config_key(String key) {
    key.trim();
    return key;
}

struct BoolConfigRuntime {
    EdfRecorderManager &edf_recorder_manager;
};

struct BoolConfigKey {
    const char *key;
    bool (*get)(const AppConfigData &cfg);
    bool (*set)(AppConfig &config, bool enabled);
    void (*apply)(AppConfig &config, BoolConfigRuntime &runtime);
};

bool get_resmed_time_sync(const AppConfigData &cfg) {
    return cfg.resmed_time_sync_enabled;
}

bool set_resmed_time_sync(AppConfig &config, bool enabled) {
    return config.set_resmed_time_sync(enabled);
}

bool get_oximetry_enabled(const AppConfigData &cfg) {
    return cfg.oximetry_enabled;
}

bool set_oximetry_enabled(AppConfig &config, bool enabled) {
    return config.set_oximetry_enabled(enabled);
}

bool get_edf_capture_enabled(const AppConfigData &cfg) {
    return cfg.edf_capture_enabled;
}

bool set_edf_capture_enabled(AppConfig &config, bool enabled) {
    return config.set_edf_capture_enabled(enabled);
}

bool get_syslog_enabled(const AppConfigData &cfg) {
    return cfg.syslog_enabled;
}

bool set_syslog_enabled(AppConfig &config, bool enabled) {
    return config.set_syslog(enabled, config.data().syslog_host,
                             config.data().syslog_port);
}

bool get_file_log_enabled(const AppConfigData &cfg) {
    return cfg.file_log_enabled;
}

bool set_file_log_enabled(AppConfig &config, bool enabled) {
    return config.set_file_log(enabled);
}

void apply_noop(AppConfig &, BoolConfigRuntime &) {}

void apply_edf_capture(AppConfig &config, BoolConfigRuntime &runtime) {
    runtime.edf_recorder_manager.set_enabled(
        config.data().edf_capture_enabled);
}

void apply_log_config(AppConfig &config, BoolConfigRuntime &) {
    config.apply_log_config();
}

const BoolConfigKey *find_bool_config_key(const String &key) {
    static const BoolConfigKey keys[] = {
        {"resmed_time", get_resmed_time_sync,
         set_resmed_time_sync, apply_noop},
        {"oxi_en", get_oximetry_enabled,
         set_oximetry_enabled, apply_noop},
        {"edf_cap", get_edf_capture_enabled,
         set_edf_capture_enabled, apply_edf_capture},
        {"syslog_en", get_syslog_enabled, set_syslog_enabled,
         apply_log_config},
        {"file_log_en", get_file_log_enabled,
         set_file_log_enabled, apply_log_config},
    };

    for (const auto &entry : keys) {
        if (key == entry.key) return &entry;
    }
    return nullptr;
}

bool parse_config_log_key(const String &key, log_cat_t &cat) {
    cat = CAT_GENERAL;
    if (!key.startsWith("log") || key.length() <= 3) return false;
    String index_text = key.substring(3);
    for (size_t i = 0; i < index_text.length(); ++i) {
        if (!isDigit(index_text[i])) return false;
    }
    const int parsed = index_text.toInt();
    if (parsed < 0 || parsed >= CAT_COUNT) return false;
    cat = static_cast<log_cat_t>(parsed);
    return true;
}

void print_config_value_header(Print &out, const char *key) {
    out.print("[CONFIG] ");
    out.print(key);
    out.print("=");
}

void print_config_value(Print &out, const char *key, const String &value) {
    print_config_value_header(out, key);
    out.println(value.length() ? value : "<empty>");
}

void print_config_secret(Print &out, const char *key, const String &value) {
    print_config_value_header(out, key);
    out.println(secret_state_text(value));
}

bool print_app_config_value(Print &out,
                            const AppConfigData &cfg,
                            String key) {
    key = trim_config_key(key);
    if (!key.length() || key.indexOf(' ') >= 0) return false;

    if (const BoolConfigKey *bool_key = find_bool_config_key(key)) {
        print_config_value_header(out, bool_key->key);
        out.println(on_off_text(bool_key->get(cfg)));
        return true;
    }
    log_cat_t log_cat = CAT_GENERAL;
    if (parse_config_log_key(key, log_cat)) {
        print_config_value_header(out, key.c_str());
        out.println(Log::level_name(cfg.log_levels[log_cat]));
        return true;
    }
    if (key == "host") {
        print_config_value(out, "host", cfg.hostname);
        return true;
    }
    if (key == "tcp_en") {
        print_config_value_header(out, "tcp_en");
        out.println(on_off_text(cfg.tcp_bridge_enabled));
        return true;
    }
    if (key == "tcp_port") {
        print_config_value_header(out, "tcp_port");
        out.println(cfg.tcp_bridge_port);
        return true;
    }
    if (key == "softap_mode") {
        print_config_value_header(out, "softap_mode");
        out.println(softap_mode_name(cfg.softap_mode));
        return true;
    }
    if (key == "wifi_ctry") {
        print_config_value(out, "wifi_ctry", cfg.wifi_country);
        return true;
    }
    if (key == "tz") {
        print_config_value(out, "tz", cfg.timezone);
        return true;
    }
    if (key == "oxi_udp") {
        print_config_value_header(out, "oxi_udp");
        out.println(cfg.oximetry_udp_port);
        return true;
    }
    if (key == "oxi_adv") {
        print_config_value_header(out, "oxi_adv");
        out.println(oximetry_advertise_mode_name(
            cfg.oximetry_advertise_mode));
        return true;
    }
    if (key == "smb_ep") {
        print_config_value(out, "smb_ep", cfg.smb_endpoint);
        return true;
    }
    if (key == "smb_user") {
        print_config_value(out, "smb_user", cfg.smb_user);
        return true;
    }
    if (key == "smb_pass") {
        print_config_secret(out, "smb_pass", cfg.smb_password);
        return true;
    }
    if (key == "shq_id") {
        print_config_value(out, "shq_id", cfg.sleephq_client_id);
        return true;
    }
    if (key == "shq_secret") {
        print_config_secret(out, "shq_secret",
                            cfg.sleephq_client_secret);
        return true;
    }
    if (key == "shq_team") {
        print_config_value(out, "shq_team", cfg.sleephq_team_id);
        return true;
    }
    if (key == "shq_device") {
        print_config_value(out, "shq_device", cfg.sleephq_device_id);
        return true;
    }
    if (key == "http_user") {
        print_config_value(out, "http_user", cfg.http_user);
        return true;
    }
    if (key == "http_pass") {
        print_config_secret(out, "http_pass", cfg.http_password);
        return true;
    }
    if (key == "auth_wl") {
        print_config_value(out, "auth_wl", cfg.auth_whitelist);
        return true;
    }
    if (key == "telnet_en") {
        print_config_value_header(out, "telnet_en");
        out.println(on_off_text(cfg.telnet_console_enabled));
        return true;
    }
    if (key == "telnet_port") {
        print_config_value_header(out, "telnet_port");
        out.println(cfg.telnet_console_port);
        return true;
    }
    if (key == "ota_pass") {
        print_config_secret(out, "ota_pass", cfg.ota_password);
        return true;
    }
    if (key == "syslog_host") {
        print_config_value(out, "syslog_host", cfg.syslog_host);
        return true;
    }
    if (key == "syslog_port") {
        print_config_value_header(out, "syslog_port");
        out.println(cfg.syslog_port);
        return true;
    }
    return false;
}

bool split_config_key_value(String rest,
                            String &key,
                            bool &has_value,
                            String &value) {
    key = "";
    value = "";
    has_value = false;

    int pos = 0;
    if (!parse_console_arg(rest, pos, key)) return false;
    key = trim_config_key(key);
    if (!key.length()) return false;

    while (pos < static_cast<int>(rest.length()) &&
           isspace(static_cast<unsigned char>(rest[pos]))) {
        ++pos;
    }
    if (pos >= static_cast<int>(rest.length())) return true;

    has_value = true;
    String tail = rest.substring(pos);
    tail.trim();
    if (!tail.length()) return true;

    if (tail[0] == '"' || tail[0] == '\'') {
        int tail_pos = 0;
        String parsed;
        if (parse_console_arg(tail, tail_pos, parsed)) {
            while (tail_pos < static_cast<int>(tail.length()) &&
                   isspace(static_cast<unsigned char>(tail[tail_pos]))) {
                ++tail_pos;
            }
            if (tail_pos >= static_cast<int>(tail.length())) {
                value = parsed;
                return true;
            }
        }
    }

    value = tail;
    return true;
}

bool parse_config_bool_value(String value, bool &enabled) {
    value.trim();
    value.toLowerCase();
    if (value == "enable" || value == "enabled") {
        enabled = true;
        return true;
    }
    if (value == "disable" || value == "disabled") {
        enabled = false;
        return true;
    }
    return parse_on_off(value, enabled);
}

void print_config_invalid(Print &out, const char *key) {
    out.print("[CONFIG] invalid ");
    out.println(key);
}

void print_config_read_only(Print &out, const char *key) {
    out.print("[CONFIG] ");
    out.print(key);
    out.println(" is read-only");
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
#if AC_STACK_PROFILE_ENABLED
    if (s.sensor_task_started) {
        out.print(" stack_free=");
        out.print(static_cast<unsigned long>(
            s.sensor_task_stack_high_water_bytes));
    }
#endif
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
        case RpcEventKind::InternalDeviceStateUpdated:
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
            out.println(event.payload_c_str());
            break;
        case RpcEventKind::RpcNotification:
            break;
        case RpcEventKind::RpcUnmatched:
            out.print("[RPC unmatched] ");
            out.println(event.payload_c_str());
            break;
        case RpcEventKind::DebugLog:
            break;
        case RpcEventKind::InternalSettingsStateInvalidated:
            break;
        case RpcEventKind::InternalSettingsStateUpdated:
            break;
        case RpcEventKind::InternalDeviceStateUpdated:
            break;
        case RpcEventKind::BootNotification:
            out.print("[CAN] ");
            out.println(event.payload_c_str());
            break;
        case RpcEventKind::FramingError:
            out.print("[FRAMING] ");
            out.println(event.payload_c_str());
            break;
        case RpcEventKind::Info:
            out.print("[INFO] ");
            out.println(event.payload_c_str());
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
    uint32_t sample_ms = 40;
    uint32_t report_ms = 200;

    if (rest == "edf" || rest == "full" || rest == "default") {
        ids = DEFAULT_EDF_STREAM_IDS;
        sample_ms = 40;
        report_ms = 200;
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
        if (ota.arduino_active) out.print("/active");
        out.print(" port=");
        out.print(ota.arduino_port);
        out.print(" auth=");
        out.print(ota.auth_enabled ? "on" : "off");
        out.print(" http=");
        out.print(ota.http_prepare_pending
                      ? "preparing"
                      : (ota.http_prepared
                             ? "prepared"
                             : (ota.http_active
                                    ? "active"
                                    : (ota.http_ready ? "ready" : "idle"))));
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

    if (rest == "abort") {
        ota_manager.abort_http_upload("aborted_by_console");
        out.println("[OTA] aborted");
        return;
    }

    print_unknown_command(out, "OTA", "ota status, abort");
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

    print_unknown_command(out, "SINK", "sink status");
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

bool ManagementConsole::handle_config_key(
    Print &out,
    String rest,
    AppConfig &app_config,
    WifiManager &wifi_manager,
    TcpBridge &tcp_bridge,
    OtaManager &ota_manager,
    EdfRecorderManager &edf_recorder_manager) {
    String key;
    String value;
    bool has_value = false;
    if (!split_config_key_value(rest, key, has_value, value)) return false;

    if (!has_value) return print_app_config_value(out, app_config.data(), key);

    bool ok = false;
    if (const BoolConfigKey *bool_key = find_bool_config_key(key)) {
        bool enabled = false;
        if (!parse_config_bool_value(value, enabled)) {
            print_config_invalid(out, bool_key->key);
            return true;
        }
        ok = bool_key->set(app_config, enabled);
        if (!ok) {
            print_config_invalid(out, bool_key->key);
            return true;
        }
        BoolConfigRuntime runtime{edf_recorder_manager};
        bool_key->apply(app_config, runtime);
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    log_cat_t log_cat = CAT_GENERAL;
    if (parse_config_log_key(key, log_cat)) {
        log_level_t level = LOG_INFO;
        if (!Log::parse_level(value, level)) {
            print_config_invalid(out, key.c_str());
            return true;
        }
        ok = app_config.set_log_level(log_cat, level);
        if (!ok) {
            print_config_invalid(out, key.c_str());
            return true;
        }
        app_config.apply_log_config();
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "host") {
        ok = app_config.set_hostname(value);
        if (!ok) {
            print_config_invalid(out, "host");
            return true;
        }
        wifi_manager.set_hostname(app_config.data().hostname);
        ota_manager.mark_config_dirty();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "tcp_en") {
        bool enabled = false;
        if (!parse_config_bool_value(value, enabled)) {
            print_config_invalid(out, "tcp_en");
            return true;
        }
        ok = app_config.set_tcp_bridge(enabled,
                                       app_config.data().tcp_bridge_port);
        if (!ok) {
            print_config_invalid(out, "tcp_en");
            return true;
        }
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "tcp_port") {
        uint16_t port = 0;
        if (!parse_uint16_arg(value, port)) {
            print_config_invalid(out, "tcp_port");
            return true;
        }
        ok = app_config.set_tcp_bridge(
            app_config.data().tcp_bridge_enabled, port);
        if (!ok) {
            print_config_invalid(out, "tcp_port");
            return true;
        }
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "softap_mode") {
        SoftApMode mode;
        if (!parse_softap_mode(value, mode)) {
            print_config_invalid(out, "softap_mode");
            return true;
        }
        const bool should_reconnect =
            wifi_manager.mode_state() == WifiModeState::SoftAp &&
            mode == SoftApMode::Auto &&
            wifi_manager.has_sta_config();
        ok = app_config.set_softap_mode(mode);
        if (!ok) {
            print_config_invalid(out, "softap_mode");
            return true;
        }
        wifi_manager.set_softap_mode(app_config.data().softap_mode);
        wifi_manager.apply_softap_mode();
        if (should_reconnect) wifi_manager.reconnect();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "wifi_ctry") {
        ok = app_config.set_wifi_country(value);
        if (!ok) {
            print_config_invalid(out, "wifi_ctry");
            return true;
        }
        wifi_manager.set_country_code(app_config.data().wifi_country);
        wifi_manager.reconnect();
        apply_runtime_config(app_config, wifi_manager, tcp_bridge);
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "tz") {
        ok = app_config.set_timezone(value);
        if (!ok) {
            print_config_invalid(out, "tz");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "oxi_udp") {
        uint16_t port = 0;
        if (!parse_uint16_arg(value, port)) {
            print_config_invalid(out, "oxi_udp");
            return true;
        }
        ok = app_config.set_oximetry_udp_port(port);
        if (!ok) {
            print_config_invalid(out, "oxi_udp");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "oxi_adv") {
        OximetryAdvertiseMode mode;
        if (!parse_oximetry_advertise_mode(value, mode)) {
            print_config_invalid(out, "oxi_adv");
            return true;
        }
        ok = app_config.set_oximetry_advertise_mode(mode);
        if (!ok) {
            print_config_invalid(out, "oxi_adv");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "smb_ep") {
        ok = app_config.set_smb_credentials(
            value, app_config.data().smb_user,
            app_config.data().smb_password);
        if (!ok) {
            print_config_invalid(out, "smb_ep");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "smb_user") {
        ok = app_config.set_smb_credentials(
            app_config.data().smb_endpoint, value,
            app_config.data().smb_password);
        if (!ok) {
            print_config_invalid(out, "smb_user");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "smb_pass") {
        ok = app_config.set_smb_credentials(
            app_config.data().smb_endpoint, app_config.data().smb_user,
            value);
        if (!ok) {
            print_config_invalid(out, "smb_pass");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "shq_id") {
        ok = app_config.set_sleephq_credentials(
            value, app_config.data().sleephq_client_secret,
            app_config.data().sleephq_team_id,
            app_config.data().sleephq_device_id);
        if (!ok) {
            print_config_invalid(out, "shq_id");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "shq_secret") {
        ok = app_config.set_sleephq_credentials(
            app_config.data().sleephq_client_id, value,
            app_config.data().sleephq_team_id,
            app_config.data().sleephq_device_id);
        if (!ok) {
            print_config_invalid(out, "shq_secret");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "shq_team") {
        ok = app_config.set_sleephq_credentials(
            app_config.data().sleephq_client_id,
            app_config.data().sleephq_client_secret, value,
            app_config.data().sleephq_device_id);
        if (!ok) {
            print_config_invalid(out, "shq_team");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "shq_device") {
        ok = app_config.set_sleephq_credentials(
            app_config.data().sleephq_client_id,
            app_config.data().sleephq_client_secret,
            app_config.data().sleephq_team_id, value);
        if (!ok) {
            print_config_invalid(out, "shq_device");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "http_user") {
        ok = app_config.set_http_auth(value,
                                      app_config.data().http_password);
        if (!ok) {
            print_config_invalid(out, "http_user");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "http_pass") {
        ok = app_config.set_http_auth(app_config.data().http_user,
                                      value);
        if (!ok) {
            print_config_invalid(out, "http_pass");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "auth_wl") {
        ok = app_config.set_auth_whitelist(value);
        if (!ok) {
            print_config_invalid(out, "auth_wl");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "telnet_en") {
        bool enabled = false;
        if (!parse_config_bool_value(value, enabled)) {
            print_config_invalid(out, "telnet_en");
            return true;
        }
        ok = app_config.set_telnet_console(
            enabled, app_config.data().telnet_console_port);
        if (!ok) {
            print_config_invalid(out, "telnet_en");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "telnet_port") {
        uint16_t port = 0;
        if (!parse_uint16_arg(value, port)) {
            print_config_invalid(out, "telnet_port");
            return true;
        }
        ok = app_config.set_telnet_console(
            app_config.data().telnet_console_enabled, port);
        if (!ok) {
            print_config_invalid(out, "telnet_port");
            return true;
        }
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "ota_pass") {
        ok = app_config.set_ota_password(value);
        if (!ok) {
            print_config_invalid(out, "ota_pass");
            return true;
        }
        ota_manager.mark_config_dirty();
        print_app_config_value(out, app_config.data(), key);
        return true;
    }

    if (key == "syslog_host") {
        ok = app_config.set_syslog(value.length() > 0, value,
                                   app_config.data().syslog_port);
        if (!ok) {
            print_config_invalid(out, "syslog_host");
            return true;
        }
        app_config.apply_log_config();
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    if (key == "syslog_port") {
        uint16_t port = 0;
        if (!parse_uint16_arg(value, port)) {
            print_config_invalid(out, "syslog_port");
            return true;
        }
        ok = app_config.set_syslog(app_config.data().syslog_enabled,
                                   app_config.data().syslog_host, port);
        if (!ok) {
            print_config_invalid(out, "syslog_port");
            return true;
        }
        app_config.apply_log_config();
        print_app_config_value(out, app_config.data(), key);
        return true;
    }
    return false;
}

void ManagementConsole::handle_config(Print &out, String rest,
                                      AppConfig &app_config,
                                      WifiManager &wifi_manager,
                                      TcpBridge &tcp_bridge,
                                      OtaManager &ota_manager,
                                      EdfRecorderManager &edf_recorder_manager) {
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

    if (handle_config_key(out, rest, app_config, wifi_manager, tcp_bridge,
                          ota_manager, edf_recorder_manager)) {
        return;
    }

    print_unknown_command(out, "CONFIG",
                          "config, config KEY [VALUE], reset, factory-reset");
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
