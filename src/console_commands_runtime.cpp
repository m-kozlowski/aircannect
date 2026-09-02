#include "console_commands.h"

#include "firmware_installer.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "live_chart_service.h"
#include "session_manager.h"
#include "therapy_telemetry_broker.h"
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

SystemConsoleCommands::SystemConsoleCommands(FirmwareInstaller &installer)
    : installer_(installer) {}

bool SystemConsoleCommands::execute(
    const String &command,
    const String &rest_arg,
    Print &out,
    ConsoleCommandSession &) {
    if (command != "restart") return false;

    String argument;
    String extra;
    int position = 0;
    const bool has_argument = parse_console_arg(rest_arg, position, argument);
    const bool has_extra = parse_console_arg(rest_arg, position, extra);

    if (!has_argument && !has_extra) {
        installer_.schedule_reboot(500);
        out.println("[SYSTEM] AirCANnect restart scheduled");
        return true;
    }

    print_unknown_command(out, "SYSTEM", "restart");
    return true;
}

RuntimeConsoleCommands::RuntimeConsoleCommands(SessionManager &session,
                                               LiveChartService &live,
                                               TherapyTelemetryBroker &telemetry)
    : session_(session), live_(live), telemetry_(telemetry) {}

bool RuntimeConsoleCommands::execute(
    const String &command,
    const String &rest_arg,
    Print &out,
    ConsoleCommandSession &console_session) {
    (void)command;
    (void)rest_arg;
    (void)out;
    (void)console_session;
    return false;
}

void RuntimeConsoleCommands::print_summary(Print &out) {
    ConsoleFormat::print_session_summary(out, session_.status());
}

void RuntimeConsoleCommands::print_status(Print &out) {
    ConsoleFormat::print_session_status(out, session_.status());
    ConsoleFormat::print_therapy_telemetry_status(out, telemetry_.status());
}

void RuntimeConsoleCommands::print_stats(Print &out) {
    ConsoleFormat::print_live_stats(out, live_);
}

void RuntimeConsoleCommands::reset_stats() {
    live_.reset_counters();
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
