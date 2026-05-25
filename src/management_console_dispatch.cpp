#include "management_console.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "debug_log.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
#include "string_util.h"
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
    const StreamBroker &stream = ctx.arbiter.stream_broker();
    const size_t frame_pool_slots = stream.frame_pool_capacity();
    const size_t frame_pool_bytes =
        frame_pool_slots * sizeof(StreamFrameData) +
        frame_pool_slots * sizeof(StreamFrameData *);
    out.print("[MEM owner] stream_frame_pool slots=");
    out.print(static_cast<unsigned long>(frame_pool_slots));
    out.print(" in_use=");
    out.print(static_cast<unsigned long>(stream.frame_pool_in_use()));
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
}

void print_heap_trace_status(Print &out) {
    const HeapTraceStatus trace = Memory::heap_trace_status();
    out.print("[MEM trace] build=");
    out.print(trace.build_enabled ? "enabled" : "disabled");
    out.print(" backend=");
    out.print(trace.backend_available ? "available" : "unavailable");
    out.print(" initialized=");
    out.print(trace.initialized ? "yes" : "no");
    out.print(" running=");
    out.print(trace.running ? "yes" : "no");
    out.print(" mode=");
    out.print(Memory::heap_trace_mode_name(trace.mode));
    out.print(" records=");
    out.print(static_cast<unsigned long>(trace.count));
    out.print('/');
    out.print(static_cast<unsigned long>(trace.capacity));
    out.print(" allocs=");
    out.print(static_cast<unsigned long>(trace.total_allocations));
    out.print(" frees=");
    out.print(static_cast<unsigned long>(trace.total_frees));
    out.print(" high_water=");
    out.print(static_cast<unsigned long>(trace.high_water_mark));
    out.print(" overflow=");
    out.print(trace.overflowed ? "yes" : "no");
    if (trace.last_error && trace.last_error[0]) {
        out.print(" error=");
        out.print(trace.last_error);
    }
    out.println();
}

bool parse_heap_trace_mode(const String &text, HeapTraceMode &mode) {
    if (!text.length() || text == "leaks") {
        mode = HeapTraceMode::Leaks;
        return true;
    }
    if (text == "all") {
        mode = HeapTraceMode::All;
        return true;
    }
    return false;
}

void print_heap_trace_records(Print &out, size_t limit) {
    const HeapTraceStatus status = Memory::heap_trace_status();
    const size_t count = status.count;
    if (limit == 0 || limit > count) limit = count;
    out.print("[MEM trace dump] records=");
    out.print(static_cast<unsigned long>(count));
    out.print(" showing=");
    out.print(static_cast<unsigned long>(limit));
    out.println();

    for (size_t i = 0; i < limit; ++i) {
        HeapTraceRecord record;
        if (!Memory::heap_trace_record(i, record)) continue;
        out.print("[MEM trace record] i=");
        out.print(static_cast<unsigned long>(i));
        out.print(" addr=0x");
        out.print(static_cast<unsigned long>(record.address), HEX);
        out.print(" size=");
        out.print(static_cast<unsigned long>(record.size));
        out.print(" freed=");
        out.print(record.freed ? "yes" : "no");
        if (record.alloc_pc) {
            out.print(" alloc_pc=0x");
            out.print(static_cast<unsigned long>(record.alloc_pc), HEX);
        }
        if (record.free_pc) {
            out.print(" free_pc=0x");
            out.print(static_cast<unsigned long>(record.free_pc), HEX);
        }
        out.println();
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
        {"oxi", &ManagementConsole::handle_oximetry_command},
        {"oximetry", &ManagementConsole::handle_oximetry_command},
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
    ConsoleFormat::print_rpc_status(out, ctx.arbiter);
    ConsoleFormat::print_as11_status(out, ctx.arbiter.as11_state());
    ConsoleFormat::print_session_status(out, ctx.session_manager.status());
    ConsoleFormat::print_sink_status(out, ctx.sink_manager);
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
        print_heap_trace_status(out);
        return;
    }
    if (rest == "trace" || rest == "trace status") {
        print_heap_trace_status(out);
        return;
    }
    if (rest.startsWith("trace start")) {
        String mode_text = rest.substring(strlen("trace start"));
        trim_inplace(mode_text);
        HeapTraceMode mode = HeapTraceMode::Leaks;
        if (!parse_heap_trace_mode(mode_text, mode)) {
            print_unknown_command(
                out, "MEM",
                "memory, memory detail, memory trace start [leaks|all]");
            return;
        }
        const bool ok = Memory::heap_trace_start(mode);
        out.print("[MEM trace] start=");
        out.print(ok ? "ok" : "failed");
        out.println();
        print_heap_trace_status(out);
        return;
    }
    if (rest == "trace stop") {
        const bool ok = Memory::heap_trace_stop();
        out.print("[MEM trace] stop=");
        out.print(ok ? "ok" : "failed");
        out.println();
        print_heap_trace_status(out);
        return;
    }
    if (rest == "trace clear") {
        const bool ok = Memory::heap_trace_clear();
        out.print("[MEM trace] clear=");
        out.print(ok ? "ok" : "failed");
        out.println();
        print_heap_trace_status(out);
        return;
    }
    if (rest == "trace dump" || rest.startsWith("trace dump ")) {
        size_t limit = 32;
        if (rest.length() > strlen("trace dump")) {
            String limit_text = rest.substring(strlen("trace dump"));
            trim_inplace(limit_text);
            char *end = nullptr;
            const unsigned long parsed = strtoul(limit_text.c_str(), &end, 10);
            if (!end || *end != 0) {
                print_unknown_command(
                    out, "MEM",
                    "memory trace dump [limit]");
                return;
            }
            limit = static_cast<size_t>(parsed);
        }
        print_heap_trace_status(out);
        print_heap_trace_records(out, limit);
        return;
    }
    print_unknown_command(
        out, "MEM",
        "memory, memory detail, memory trace start|stop|dump|clear");
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

void ManagementConsole::handle_oximetry_command(Print &out,
                                                String rest,
                                                ConsoleContext &ctx) {
    handle_oximetry(out, rest, ctx.oximetry_manager);
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
