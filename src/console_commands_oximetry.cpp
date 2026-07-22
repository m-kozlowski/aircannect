#include "console_commands.h"

#include "ble_sensor_source.h"
#include "board.h"
#include "config_service.h"
#include "management_console_utils.h"
#include "oximetry_hub.h"
#include "oximetry_status.h"
#include "plx_peripheral.h"
#include "udp_oximeter_source.h"

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

void print_oximetry_status(Print &out,
                           const OximetryHub &hub,
                           const UdpOximeterSource &udp,
                           const BleSensorSource &sensor_source,
                           const PlxPeripheral &peripheral) {
    const uint32_t now_ms = millis();
    const OximetryRuntimeStatus status = compose_oximetry_status(
        hub.snapshot(now_ms), udp.status(), peripheral.status(now_ms));
    const BleSensorStatus sensor = sensor_source.status();

    out.print("[OXI] enabled=");
    print_yes_no(out, status.enabled);
    out.print(" source=");
    out.print(oximetry_source_text(status.source));
    if (status.source_detail[0]) {
        out.print(':');
        out.print(status.source_detail);
    }
    out.print(" present=");
    print_yes_no(out, status.source_present);
    out.print(" fresh=");
    print_yes_no(out, status.source_fresh);
    out.print(" valid=");
    print_yes_no(out, status.reading.valid);
    out.print(" spo2=");
    if (status.reading.valid) out.print(status.reading.spo2);
    else out.print("--");
    out.print(" pulse=");
    if (status.reading.valid) out.print(status.reading.pulse_bpm);
    else out.print("--");
    out.print(" age_ms=");
    out.print(status.last_source_age_ms);
    out.print(" udp=");
    out.print(status.udp_started ? "listening" : "stopped");
    out.print(':');
    out.print(status.udp_port);
    out.print(" packets=");
    out.print(status.udp_packets);
    out.print('/');
    out.print(status.udp_bad_packets);
    out.print(" advertise=");
    out.print(oximetry_advertise_mode_name(status.advertise_mode));
    out.print(" pair=");
    if (status.pairing_active) {
        out.print("active/");
        out.print((status.pairing_left_ms + 999) / 1000);
        out.print('s');
    } else {
        out.print("off");
    }
    out.print(" ble=");
    out.print(status.ble_available ? "available" : "disabled");
    out.print(" adv=");
    print_yes_no(out, status.advertising);
    out.print(" connected=");
    print_yes_no(out, status.connected);
    out.print(" subscribed=");
    print_yes_no(out, status.subscribed);
    out.print(" disconnect_reason=");
    out.print(status.ble_last_disconnect_reason);
    out.print(" sensor=");
    out.print(sensor_state_text(sensor.state));
    out.print(" known=");
    out.print(sensor.known_count);
    out.print(" scan=");
    out.print(sensor.scan_count);
    if (sensor.peer[0]) {
        out.print(" peer=");
        out.print(sensor.peer);
    }
    out.print(" name=\"");
    out.print(status.ble_name);
    out.println("\"");
}

void print_sensor_status(Print &out, const BleSensorSource &sensor) {
    const BleSensorStatus status = sensor.status();
    out.print("[OXI sensor] state=");
    out.print(sensor_state_text(status.state));
    out.print(" task=");
    print_yes_no(out, status.task_started);
#if AC_STACK_PROFILE_ENABLED
    if (status.task_started) {
        out.print(" stack_free=");
        out.print(static_cast<unsigned long>(status.task_stack_high_water_bytes));
    }
#endif
    out.print(" scanning=");
    print_yes_no(out, status.scanning);
    out.print(" connected=");
    print_yes_no(out, status.connected);
    if (status.peer[0]) {
        out.print(" peer=");
        out.print(status.peer);
    }
    if (status.name[0]) {
        out.print(" name=\"");
        out.print(status.name);
        out.print('"');
    }
    out.print(" known=");
    out.print(status.known_count);
    out.print(" results=");
    out.print(status.scan_count);
    out.print(" notifications=");
    out.print(status.notifications);
    out.print(" invalid=");
    out.print(status.invalid_notifications);
    out.print(" connects=");
    out.print(status.connects);
    out.print(" disconnects=");
    out.print(status.disconnects);
    out.print(" failures=");
    out.println(status.connect_failures);
}

void print_scan_results(Print &out, const BleSensorSource &sensor) {
    out.println("[OXI sensor scan]");
    OximetrySensorDevice snapshot[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    const size_t count = sensor.scan_results(
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

void print_known_sensors(Print &out, const BleSensorSource &sensor) {
    out.println("[OXI sensor known]");
    OximetrySensorDevice snapshot[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    const size_t count = sensor.known_sensors(
        snapshot, AC_OXIMETRY_SENSOR_MAX_KNOWN);
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

OximetryConsoleCommands::OximetryConsoleCommands(OximetryHub &hub,
                                                 UdpOximeterSource &udp,
                                                 BleSensorSource &sensor,
                                                 PlxPeripheral &peripheral,
                                                 ConfigService &config)
    : hub_(hub),
      udp_(udp),
      sensor_(sensor),
      peripheral_(peripheral),
      config_(config) {}

bool OximetryConsoleCommands::execute(const String &command,
                                      const String &rest_arg,
                                      Print &out,
                                      ConsoleCommandSession &session) {
    (void)session;
    if (command != "oxi" && command != "oximetry") return false;

    String rest = rest_arg;
    rest.trim();
    String lower = rest;
    lower.toLowerCase();
    if (!lower.length() || lower == "status") {
        print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
        return true;
    }

    if (lower == "on" || lower == "enable" || lower == "enabled" ||
        lower == "off" || lower == "disable" || lower == "disabled") {
        const bool enabled = lower == "on" || lower == "enable" ||
                             lower == "enabled";
        ConfigTransactionResult transaction;
        const ConfigFieldUpdate update = config_.set_value(
            "oxi_en", enabled ? "1" : "0", false, &transaction);
        if (!update.accepted() || !transaction.persisted) {
            out.println(enabled ? "[OXI] failed to enable"
                                : "[OXI] failed to disable");
            return true;
        }
        out.println(enabled ? "[OXI] enabled" : "[OXI] disabled");
        print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
        return true;
    }

    if (lower == "cpap pair" || lower == "cpap pairing" ||
        lower == "cpap pair start" || lower == "cpap pairing start") {
        ConfigTransactionResult transaction;
        const ConfigFieldUpdate update = config_.set_value(
            "oxi_en", "1", false, &transaction);
        if (!update.accepted() || !transaction.persisted) {
            out.println("[OXI] failed to enable");
            return true;
        }
        peripheral_.request_pairing(true);
        out.println("[OXI] CPAP pairing window started");
        print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
        return true;
    }
    if (lower == "cpap pair stop" || lower == "cpap pairing stop") {
        peripheral_.request_pairing(false);
        out.println("[OXI] CPAP pairing window stopped");
        print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
        return true;
    }
    if (lower == "cpap forget" || lower == "cpap forget-bonds") {
        out.println(peripheral_.forget_bonds()
                        ? "[OXI] CPAP BLE bonds cleared"
                        : "[OXI] CPAP BLE bond clear failed");
        return true;
    }
    if (lower == "cpap status") {
        print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
        return true;
    }

    if (lower == "sensor" || lower == "sensor status") {
        print_sensor_status(out, sensor_);
        return true;
    }
    if (lower == "sensor scan") {
        out.println(sensor_.request_scan()
                        ? "[OXI sensor] scan queued"
                        : "[OXI sensor] scan failed");
        print_sensor_status(out, sensor_);
        return true;
    }
    if (lower == "sensor results" || lower == "sensor scan-results") {
        print_scan_results(out, sensor_);
        return true;
    }
    if (lower == "sensor list" || lower == "sensor known") {
        print_known_sensors(out, sensor_);
        return true;
    }
    if (lower == "sensor disconnect") {
        sensor_.request_disconnect();
        out.println("[OXI sensor] disconnect queued");
        print_sensor_status(out, sensor_);
        return true;
    }
    if (lower.startsWith("sensor connect ")) {
        String target = rest.substring(15);
        target.trim();
        if (!sensor_.request_connect(target.c_str())) {
            out.println(
                "[OXI sensor] connect failed; use scan result index or "
                "known address");
        } else {
            out.println("[OXI sensor] connect queued");
            print_sensor_status(out, sensor_);
        }
        return true;
    }
    if (lower.startsWith("sensor forget ")) {
        String target = rest.substring(14);
        target.trim();
        if (!sensor_.forget(target.c_str())) {
            out.println("[OXI sensor] forget failed");
        } else {
            out.println("[OXI sensor] forgotten");
            print_known_sensors(out, sensor_);
        }
        return true;
    }
    if (lower.startsWith("sensor autoconnect ")) {
        String args = rest.substring(19);
        args.trim();
        const int split = args.lastIndexOf(' ');
        if (split <= 0) {
            out.println(
                "[OXI sensor] usage: oxi sensor autoconnect ADDR on|off");
            return true;
        }

        String address = args.substring(0, split);
        String value = args.substring(split + 1);
        bool enabled = false;
        if (!parse_on_off(value, enabled) ||
            !sensor_.set_autoconnect(address.c_str(), enabled)) {
            out.println("[OXI sensor] autoconnect failed");
        } else {
            out.print("[OXI sensor] autoconnect=");
            out.println(on_off_text(enabled));
            print_known_sensors(out, sensor_);
        }
        return true;
    }

    if (lower.startsWith("advertise ")) {
        String mode = lower.substring(10);
        mode.trim();
        if (mode == "start" || mode == "on" ||
            mode == "stop" || mode == "off") {
            const bool enabled = mode == "start" || mode == "on";
            peripheral_.request_advertising(enabled);
            out.println(enabled ? "[OXI] manual advertising requested"
                                : "[OXI] manual advertising stopped");
            print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
            return true;
        }

        OximetryAdvertiseMode advertise_mode;
        if (!parse_oximetry_advertise_mode(mode, advertise_mode)) {
            out.println("[OXI] usage: oxi advertise auto|manual|start|stop");
            return true;
        }

        ConfigTransactionResult transaction;
        const ConfigFieldUpdate update = config_.set_value(
            "oxi_adv", oximetry_advertise_mode_name(advertise_mode), false,
            &transaction);
        if (!update.accepted() || !transaction.persisted) {
            out.println("[OXI] failed to store advertise mode");
        } else {
            out.print("[OXI] advertise=");
            out.println(oximetry_advertise_mode_name(advertise_mode));
        }
        return true;
    }

    if (lower == "forget" || lower == "forget-bonds") {
        out.println("[OXI] use: oxi cpap forget");
        return true;
    }

    print_unknown_command(out, "OXI",
                          "oxi status, on, off, cpap, sensor, advertise");
    return true;
}

void OximetryConsoleCommands::print_status(Print &out) {
    print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
}

void OximetryConsoleCommands::print_stats(Print &out) {
    print_oximetry_status(out, hub_, udp_, sensor_, peripheral_);
}

void OximetryConsoleCommands::print_memory_detail(Print &out) {
    const BleSensorStatus status = sensor_.status();
    out.print("[MEM owner] oximetry_sensor task=");
    out.print(status.task_started ? "started" : "stopped");
#if AC_STACK_PROFILE_ENABLED
    if (status.task_started) {
        out.print(" stack_free=");
        out.print(static_cast<unsigned long>(status.task_stack_high_water_bytes));
    }
#endif
    out.println();
}

}  // namespace aircannect
