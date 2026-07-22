#include "console_commands.h"

#include "as11_device_service.h"
#include "debug_log.h"
#include "firmware_installer.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "memory_manager.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "tls_memory.h"
#include "web_ui.h"

namespace aircannect {
namespace {

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
    out.println(static_cast<unsigned long>(buffer.capacity));
}

void print_web_memory_detail(Print &out, WebUI &web_ui) {
    const WebUiMemoryStatus web = web_ui.memory_status();
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
    out.println(static_cast<unsigned long>(web.console_log_length));

    print_web_buffer_memory(out, "status", web.status, total_capacity);
    print_web_buffer_memory(out, "stream", web.stream, total_capacity);
    print_web_buffer_memory(out, "console", web.console, total_capacity);
    print_web_buffer_memory(out, "live", web.live, total_capacity);
    out.print("[MEM web] buffer_cap_total=");
    out.println(static_cast<unsigned long>(total_capacity));
}

}  // namespace

bool CoreDiagnosticsConsoleCommands::execute(
    const String &command,
    const String &rest,
    Print &out,
    ConsoleCommandSession &session) {
    (void)command;
    (void)rest;
    (void)out;
    (void)session;
    return false;
}

void CoreDiagnosticsConsoleCommands::print_stats(Print &out) {
    ConsoleFormat::print_log_stats(out);
    ConsoleFormat::print_memory_status(out, Memory::status());
}

void CoreDiagnosticsConsoleCommands::print_memory_detail(Print &out) {
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
    out.println(static_cast<unsigned long>(tls.frees));
}

SystemConsoleCommands::SystemConsoleCommands(FirmwareInstaller &installer,
                                             RpcRequestPort &rpc,
                                             As11DeviceService &device)
    : installer_(installer), rpc_(rpc), device_(device) {}

bool SystemConsoleCommands::execute(
    const String &command,
    const String &rest_arg,
    Print &out,
    ConsoleCommandSession &) {
    if (command != "restart") return false;

    String target;
    String mode;
    String extra;
    int position = 0;
    const bool has_target = parse_console_arg(rest_arg, position, target);
    const bool has_mode = parse_console_arg(rest_arg, position, mode);
    const bool has_extra = parse_console_arg(rest_arg, position, extra);
    target.toLowerCase();
    mode.toLowerCase();

    if ((!has_target || target == "ac") && !has_mode && !has_extra) {
        installer_.schedule_reboot(500);
        out.println("[SYSTEM] AirCANnect restart scheduled");
        return true;
    }

    if (target != "fg" || has_extra) {
        print_unknown_command(
            out, "SYSTEM", "restart [ac|fg [fast|power|wd]]");
        return true;
    }

    As11ResetMode reset_mode = As11ResetMode::Fast;
    const char *reset_type = "Fast";
    if (has_mode && mode == "power") {
        reset_mode = As11ResetMode::PowerLoss;
        reset_type = "TriggerPowerLoss";
    } else if (has_mode && mode == "wd") {
        reset_mode = As11ResetMode::Watchdog;
        reset_type = "TriggerWatchdog";
    } else if (has_mode && mode != "fast") {
        print_unknown_command(
            out, "SYSTEM", "restart [ac|fg [fast|power|wd]]");
        return true;
    }

    const OperationSubmission submitted = device_.request_reset(
        rpc_, reset_mode, RpcSource::Console);
    if (submitted.accepted()) {
        out.print("[AS11] ResetDevice queued type=");
        out.println(reset_type);
    } else if (submitted.admission == OperationAdmission::Busy) {
        out.println("[AS11] ResetDevice already pending");
    } else {
        out.println("[AS11] ResetDevice queue failed");
    }
    return true;
}

RuntimeConsoleCommands::RuntimeConsoleCommands(SessionManager &session,
                                               SinkManager &sink)
    : session_(session), sink_(sink) {}

bool RuntimeConsoleCommands::execute(
    const String &command,
    const String &rest_arg,
    Print &out,
    ConsoleCommandSession &console_session) {
    (void)console_session;
    if (command != "session" && command != "sink") return false;

    String rest = rest_arg;
    rest.trim();
    rest.toLowerCase();

    if (command == "session") {
        if (rest.length() && rest != "status") {
            print_unknown_command(out, "SESSION", "session status");
        } else {
            ConsoleFormat::print_session_status(out, session_.status());
        }
        return true;
    }

    if (command == "sink") {
        if (rest.length() && rest != "status") {
            print_unknown_command(out, "SINK", "sink status");
        } else {
            ConsoleFormat::print_sink_status(out, sink_);
        }
        return true;
    }

    return false;
}

void RuntimeConsoleCommands::print_status(Print &out) {
    ConsoleFormat::print_session_status(out, session_.status());
    ConsoleFormat::print_sink_status(out, sink_);
}

void RuntimeConsoleCommands::print_stats(Print &out) {
    print_status(out);
}

WebDiagnosticsConsoleCommands::WebDiagnosticsConsoleCommands(WebUI &web_ui)
    : web_ui_(web_ui) {}

bool WebDiagnosticsConsoleCommands::execute(
    const String &command,
    const String &rest,
    Print &out,
    ConsoleCommandSession &session) {
    (void)command;
    (void)rest;
    (void)out;
    (void)session;
    return false;
}

void WebDiagnosticsConsoleCommands::print_memory_detail(Print &out) {
    print_web_memory_detail(out, web_ui_);
}

}  // namespace aircannect
