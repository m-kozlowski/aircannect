#include "console_commands.h"

#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#include "as11_device_service.h"
#include "as11_ble_rpc_link.h"
#include "as11_rpc.h"
#include "as11_settings.h"
#include "as11_settings_manager.h"
#include "can_driver.h"
#include "can_rpc_link.h"
#include "event_broker.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "rpc_request_port.h"
#include "rpc_transport_ports.h"
#include "stream_broker.h"
#include "string_util.h"
#include "time_sync_service.h"

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

std::string cli_set_value_literal(String value) {
    value.trim();
    String lower = value;
    lower.toLowerCase();
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
    if (!first) out += ',';
    out += '"';
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

    const size_t length = object.size() - 2;
    if (!length) return;
    if (!first) out += ',';
    out.append(object, 1, length);
    first = false;
}

void print_stream_memory_detail(Print &out, const StreamBroker &stream) {
    const size_t frame_pool_slots = stream.frame_pool_capacity();
    const size_t frame_pool_bytes =
        frame_pool_slots * sizeof(StreamFrameData) +
        frame_pool_slots * sizeof(StreamFrameData *);

    const size_t queue_slots =
        AC_STREAM_CONSUMERS_MAX * AC_STREAM_CONSUMER_QUEUE_DEPTH;
    const size_t queue_bytes =
        AC_STREAM_CONSUMERS_MAX *
        sizeof(FixedQueue<StreamFrameRef, AC_STREAM_CONSUMER_QUEUE_DEPTH>);

    out.print("[MEM owner] stream_queues slots=");
    out.print(static_cast<unsigned long>(queue_slots));
    out.print(" bytes=");
    out.println(static_cast<unsigned long>(queue_bytes));

    out.print("[MEM owner] stream_frame_pool slots=");
    out.print(static_cast<unsigned long>(frame_pool_slots));
    out.print(" approx_bytes=");
    out.println(static_cast<unsigned long>(frame_pool_bytes));
}

void handle_time(Print &out,
                 String rest,
                 As11DeviceService &device,
                 TimeSyncService &time_sync) {
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_time_status(
            out, device.state(), time_sync);
        return;
    }

    if (rest != "ntp" && device.unavailable()) {
        out.println("[TIME] AS11 unavailable");
        return;
    }

    if (rest == "get") {
        (void)time_sync.request_pull_resmed_to_esp(RpcSource::Console);
        return;
    }
    if (rest == "set" || rest == "push" || rest == "sync-to-resmed") {
        if (time_sync.request_push_esp_to_resmed(RpcSource::Console)) {
            out.println("[TIME] SetDateTime queued");
        } else {
            out.println("[TIME] ESP clock is not ready or queue is full");
        }
        return;
    }
    if (rest == "pull" || rest == "sync-from-resmed") {
        if (time_sync.request_pull_resmed_to_esp(RpcSource::Console)) {
            out.println("[TIME] GetDateTime queued for ESP clock sync");
        } else {
            out.println("[TIME] GetDateTime queue failed");
        }
        return;
    }
    if (rest == "ntp") {
        time_sync.force_ntp_sync();
        out.println("[TIME] NTP resync triggered");
        return;
    }

    print_unknown_command(out, "TIME", "time, get, push, pull, ntp");
}

void handle_therapy(Print &out,
                    String rest,
                    As11DeviceService &device,
                    RpcRequestPort &rpc) {
    rest.trim();
    rest.toLowerCase();

    As11TherapyTarget target;
    const char *queued_message = nullptr;
    const char *failed_message = nullptr;
    if (rest == "start" || rest == "on" || rest == "run") {
        target = As11TherapyTarget::Running;
        queued_message = "[THERAPY] EnterTherapy queued";
        failed_message = "[THERAPY] EnterTherapy queue failed";
    } else if (rest == "stop" || rest == "off" || rest == "standby") {
        target = As11TherapyTarget::Standby;
        queued_message = "[THERAPY] EnterStandby queued";
        failed_message = "[THERAPY] EnterStandby queue failed";
    } else {
        print_unknown_command(out, "AS11", "as11 therapy start, stop");
        return;
    }

    if (device.unavailable()) {
        out.println("[THERAPY] AS11 unavailable");
        return;
    }

    const bool accepted = device.request_therapy(
        rpc, target, RpcSource::Console, millis()).accepted();
    out.println(accepted ? queued_message : failed_message);
}

void print_as11_ble_status(Print &out, const As11BleRpcLink &link) {
    const As11BleLinkStatus link_status = link.ble_status();
    const As11BlePairingStatus pairing = link.pairing_status();

    out.print("[AS11 BLE] link=");
    out.print(as11_ble_link_state_name(link_status.state));
    out.print(" pairing=");
    out.print(as11_ble_pairing_state_name(pairing.state));
    out.print(" paired=");
    out.print(pairing.paired ? "yes" : "no");
    out.print(" connected=");
    out.print(link_status.connected ? "yes" : "no");
    if (pairing.selected_address[0]) {
        out.print(" selected=");
        out.print(pairing.selected_address);
    }
    if (pairing.error[0]) {
        out.print(" error=");
        out.print(pairing.error);
    }
    out.println();

    for (size_t i = 0; i < pairing.device_count; ++i) {
        out.print("[AS11 BLE device] address=");
        out.print(pairing.devices[i].address);
        out.print(" name=\"");
        out.print(pairing.devices[i].name);
        out.print("\" rssi=");
        out.println(pairing.devices[i].rssi);
    }
}

void handle_as11_ble(
    Print &out,
    String rest,
    As11BleRpcLink &link,
    As11DeviceConsoleCommands::BleConnectionCommand connect_ble,
    As11DeviceConsoleCommands::BleConnectionCommand disconnect_ble,
    void *connection_context) {
    rest.trim();
    if (!rest.length() || rest == "status") {
        print_as11_ble_status(out, link);
        return;
    }

    bool accepted = false;
    if (rest == "pair" || rest == "scan") {
        accepted = link.request_pairing_scan();
    } else if (rest == "connect") {
        accepted = connect_ble && connect_ble(connection_context, millis());
    } else if (rest == "disconnect") {
        accepted = disconnect_ble &&
                   disconnect_ble(connection_context, millis());
    } else if (rest == "cancel") {
        accepted = link.cancel_pairing();
    } else if (rest == "forget") {
        accepted = link.forget_pairing();
    } else if (rest.startsWith("select ")) {
        String address = rest.substring(7);
        address.trim();
        accepted = link.request_pairing_device(address.c_str());
    } else if (rest.startsWith("passkey ")) {
        String passkey = rest.substring(8);
        passkey.trim();
        accepted = link.submit_pairing_passkey(passkey.c_str());
    } else {
        print_unknown_command(
            out, "AS11 BLE",
            "as11 ble status, connect, disconnect, pair, select ADDRESS, "
            "passkey CODE, cancel, forget");
        return;
    }

    out.println(accepted ? "[AS11 BLE] command queued"
                         : "[AS11 BLE] command rejected");
}

}  // namespace

As11DeviceConsoleCommands::As11DeviceConsoleCommands(
    RpcRequestPort &rpc,
    RpcPassthroughPort &passthrough,
    As11DeviceService &device,
    As11SettingsManager &settings,
    TimeSyncService &time_sync,
    As11BleRpcLink &ble_link,
    BleConnectionCommand connect_ble,
    BleConnectionCommand disconnect_ble,
    void *ble_connection_context)
    : rpc_(rpc),
      passthrough_(passthrough),
      device_(device),
      settings_(settings),
      time_sync_(time_sync),
      ble_link_(ble_link),
      connect_ble_(connect_ble),
      disconnect_ble_(disconnect_ble),
      ble_connection_context_(ble_connection_context) {}

bool As11DeviceConsoleCommands::execute(
    const String &command,
    const String &rest_arg,
    Print &out,
    ConsoleCommandSession &) {
    if (command != "as11" && command != "time") {
        return false;
    }

    String rest = rest_arg;

    if (command == "as11") {
        rest.trim();
        const int split = rest.indexOf(' ');
        String subcommand = split < 0 ? rest : rest.substring(0, split);
        String args = split < 0 ? "" : rest.substring(split + 1);
        subcommand.toLowerCase();
        args.trim();

        if (!subcommand.length() || subcommand == "status") {
            ConsoleFormat::print_as11_status(out, device_.state());
        } else if (subcommand == "ble") {
            handle_as11_ble(out, args, ble_link_, connect_ble_,
                            disconnect_ble_, ble_connection_context_);
        } else if (subcommand == "therapy") {
            handle_therapy(out, args, device_, rpc_);
        } else if (subcommand == "poll" || subcommand == "refresh") {
            device_.request_healthcheck(rpc_, RpcSource::Console, millis());
            out.println("[AS11] healthcheck scheduled");
        } else if (subcommand == "version") {
            if (device_.unavailable()) {
                out.println("[AS11] unavailable");
            } else {
                passthrough_.send_request("GetVersion", "",
                                          RpcSource::Console);
            }
        } else if (subcommand == "get" || subcommand == "set" ||
                   subcommand == "rpc" || subcommand == "raw") {
            return execute_rpc(subcommand, args, out);
        } else {
            print_unknown_command(
                out, "AS11",
                "as11 status, poll, version, get, set, rpc, raw, ble, therapy");
        }
        return true;
    }

    if (command == "time") {
        handle_time(out, rest, device_, time_sync_);
        return true;
    }

    return false;
}

void As11DeviceConsoleCommands::print_summary(Print &out) {
    ConsoleFormat::print_as11_summary(out, device_.state());
    ConsoleFormat::print_time_summary(out, device_.state(), time_sync_);
}

void As11DeviceConsoleCommands::print_status(Print &out) {
    ConsoleFormat::print_as11_status(out, device_.state());
    ConsoleFormat::print_time_status(out, device_.state(), time_sync_);
}

bool As11DeviceConsoleCommands::execute_rpc(const String &command,
                                            String rest,
                                            Print &out) {
    if (command == "get") {
        rest.trim();
        if (!rest.length()) {
            out.println("[RPC] usage: as11 get NAME [NAME...]");
        } else {
            passthrough_.send_request("Get", build_get_params(to_std(rest)),
                                      RpcSource::Console);
        }
        return true;
    }

    if (command == "set") {
        rest.trim();
        if (!rest.length()) {
            out.println(
                "[RPC] usage: as11 set NAME VALUE [NAME VALUE...] | "
                "as11 set {JSON_PARAMS}");
            return true;
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
                        "[RPC] usage: as11 set NAME VALUE [NAME VALUE...] | "
                        "as11 set {JSON_PARAMS}");
                    return true;
                }

                if (key.startsWith("_")) {
                    append_cli_set_pair(raw_params, raw_first, key, value);
                    raw_count++;
                } else {
                    append_cli_set_pair(setting_body, setting_first, key,
                                        value);
                    setting_count++;
                }
            }
            raw_params += '}';
            setting_body += '}';

            const As11SettingsState &settings = settings_.state();
            const As11DeviceState &as11 = device_.state();
            int mode = settings.mode_index();
            if (mode < 0) {
                mode = as11_mode_index_from_value(
                    as11.active_therapy_profile());
            }

            size_t accepted = 0;
            std::string mapped_params = "{}";
            if (setting_count) {
                mapped_params = as11_build_set_params_from_json(
                    setting_body, mode, accepted, settings.catalog());
            }
            if (!raw_count && !accepted) {
                out.println("[RPC] no accepted settings");
                return true;
            }
            managed_settings = accepted != 0;

            bool first = true;
            params = '{';
            append_json_object_members(params, first, raw_params);
            append_json_object_members(params, first, mapped_params);
            params += '}';
        }

        const bool queued = managed_settings
            ? settings_.write(rpc_, params, RpcSource::Console,
                              millis()).accepted()
            : passthrough_.send_request("Set", params, RpcSource::Console);
        out.println(queued ? "[RPC] Set queued" : "[RPC] Set queue failed");
        return true;
    }

    if (command == "rpc") {
        rest.trim();
        const int split = rest.indexOf(' ');
        String method = split < 0 ? rest : rest.substring(0, split);
        String params = split < 0 ? "" : rest.substring(split + 1);
        params.trim();
        if (!method.length()) {
            out.println("[RPC] usage: as11 rpc METHOD [JSON_PARAMS]");
        } else {
            passthrough_.send_request(to_std(method), to_std(params),
                                      RpcSource::Console);
        }
        return true;
    }

    if (command == "raw") {
        rest.trim();
        if (!rest.length()) {
            out.println("[RPC] usage: as11 raw JSON");
        } else {
            passthrough_.submit_raw_payload(to_std(rest), RpcSource::Console);
        }
        return true;
    }

    return false;
}

CanConsoleCommands::CanConsoleCommands(
    RpcDiagnosticsPort &diagnostics,
    CanControlPort &can_control,
    CanDriver &can,
    EventBroker &events,
    StreamBroker &stream)
    : diagnostics_(diagnostics),
      can_control_(can_control),
      can_(can),
      events_(events),
      stream_(stream) {}

bool CanConsoleCommands::execute(const String &command,
                                 const String &rest_arg,
                                 Print &out,
                                 ConsoleCommandSession &) {
    if (command != "can") return false;

#if !AC_CAN_ENABLED
    out.println("[CAN] unavailable in this build");
    return true;
#endif

    String rest = rest_arg;

    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        if (!can_control_.can_available()) {
            out.println("[CAN] unavailable for selected AS11 transport");
            return true;
        }
        ConsoleFormat::print_rpc_status(out, diagnostics_, can_);
    } else if (rest == "restart") {
        if (!can_control_.recover_can("console CAN restart command")) {
            out.println("[CAN] restart unavailable");
        }
    } else {
        print_unknown_command(out, "CAN", "can status, can restart");
    }
    return true;
}

void CanConsoleCommands::print_status(Print &out) {
#if !AC_CAN_ENABLED
    out.println("[CAN] unavailable in this build");
    return;
#endif

    if (!can_control_.can_available()) {
        out.println("[CAN] unavailable for selected AS11 transport");
        return;
    }
    ConsoleFormat::print_rpc_status(out, diagnostics_, can_);
}

void CanConsoleCommands::print_stats(Print &out) {
    ConsoleFormat::print_rpc_stats(out, diagnostics_, can_, events_, stream_);
}

void CanConsoleCommands::reset_stats() {
    diagnostics_.reset_stats();
    can_.reset_stats();
    events_.reset_counters();
    stream_.reset_counters();
}

void CanConsoleCommands::print_memory_detail(Print &out) {
    print_stream_memory_detail(out, stream_);
}

}  // namespace aircannect
