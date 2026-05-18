#include "management_console.h"

#include <ctype.h>
#include <stdlib.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "debug_log.h"
#include "management_console_utils.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
#include "string_util.h"
#include "version.h"

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
        {"storage", &ManagementConsole::handle_storage_command},
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
    ctx.arbiter.print_status(out);
    ctx.arbiter.print_as11_status(out);
    ctx.session_manager.print_status(out);
    ctx.sink_manager.print_status(out);
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
    ctx.arbiter.print_stats(out);
    ctx.tcp_bridge.print_stats(out);
    Log::print_stats(out);
    Memory::print_status(out);
    Storage::print_status(out);
    StorageWriter::print_status(out);
    ctx.session_manager.print_status(out);
    ctx.sink_manager.print_status(out);
}

void ManagementConsole::handle_memory_command(Print &out,
                                              String rest,
                                              ConsoleContext &ctx) {
    (void)ctx;
    trim_inplace(rest);
    if (rest.length()) {
        print_unknown_command(out, "MEM", "memory");
        return;
    }
    Memory::print_status(out);
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
    ctx.session_manager.print_status(out);
}

void ManagementConsole::handle_sink_command(Print &out,
                                            String rest,
                                            ConsoleContext &ctx) {
    handle_sink(out, rest, ctx.sink_manager);
}

void ManagementConsole::handle_storage_command(Print &out,
                                               String rest,
                                               ConsoleContext &ctx) {
    (void)ctx;
    trim_inplace(rest);
    String rest_lower = rest;
    to_lower_inplace(rest_lower);
    if (!rest_lower.length() || rest_lower == "status") {
        Storage::print_status(out);
        StorageWriter::print_status(out);
        return;
    }
    if (rest_lower == "remount" || rest_lower == "retry") {
        Storage::remount();
        Storage::print_status(out);
        return;
    }
    if (rest_lower == "queue" || rest_lower == "writer") {
        StorageWriter::print_status(out);
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
        StorageWriter::print_status(out);
        return;
    }
    print_unknown_command(out, "STORAGE",
                          "storage status, remount, queue, write-test");
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
                  ctx.tcp_bridge, ctx.ota_manager);
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
    ctx.tcp_bridge.print_status(out);
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
        ctx.arbiter.print_status(out);
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
