#include "management_console.h"

#include "management_console_format.h"
#include "management_console_utils.h"
#include "memory_manager.h"
#include "rpc_transport_ports.h"
#include "string_util.h"
#include "version.h"

namespace aircannect {

void ManagementConsole::begin(Print &out) {
    out.println("[CLI] ready. Type 'help'.");
}

void ManagementConsole::stop(ConsoleCommandRouter &router) {
    router.stop(session_);
    line_ = "";
}

void ManagementConsole::poll(Stream &input,
                             Print &out,
                             ConsoleCommandRouter &router) {
    poll_pending(out, router);

    while (input.available()) {
        const char c = static_cast<char>(input.read());
        if (c == '\r' || c == '\n') {
            if (line_.length()) {
                execute_line(line_, out, router);
                line_ = "";
            }
        } else if (c == '\b' || c == 0x7F) {
            if (line_.length()) line_.remove(line_.length() - 1);
        } else if (line_.length() < 1024) {
            line_ += c;
        }
    }
}

void ManagementConsole::poll_pending(Print &out,
                                     ConsoleCommandRouter &router) {
    router.poll_pending(out, session_);
}

void ManagementConsole::cancel_pending(ConsoleCommandRouter &router) {
    router.cancel_pending(session_);
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
            return false;
    }
    return false;
}

void ManagementConsole::handle_event(Print &out, const RpcEvent &event) {
    if (!event_has_output(event)) return;

    switch (event.kind) {
        case RpcEventKind::RpcResponse:
            out.print("[RPC response] ");
            out.println(event.payload_c_str());
            break;
        case RpcEventKind::RpcUnmatched:
            out.print("[RPC unmatched] ");
            out.println(event.payload_c_str());
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
        case RpcEventKind::RpcNotification:
        case RpcEventKind::DebugLog:
            break;
    }
}

void ManagementConsole::execute_line(String line,
                                     Print &out,
                                     ConsoleCommandRouter &router) {
    trim_inplace(line);
    if (!line.length()) return;

    int pos = 0;
    String command;
    if (!parse_console_arg(line, pos, command)) return;
    to_lower_inplace(command);
    String rest = pos < static_cast<int>(line.length())
                      ? line.substring(pos)
                      : "";

    if (command == "help" || command == "?") {
        trim_inplace(rest);
        print_help(out, rest);
        return;
    }

    if (command == "status") {
        trim_inplace(rest);
        if (rest.length()) {
            print_unknown_command(out, "STATUS", "status");
            return;
        }
        router.print_status(out);
        return;
    }

    if (command == "stats") {
        trim_inplace(rest);
        to_lower_inplace(rest);
        if (rest == "reset") {
            router.reset_stats();
            out.println("[STATS] reset");
            return;
        }
        if (rest.length() && rest != "status") {
            print_unknown_command(out, "STATS", "stats, stats reset");
            return;
        }
        router.print_stats(out);
        return;
    }

    if (command == "memory" || command == "mem") {
        trim_inplace(rest);
        to_lower_inplace(rest);
        if (!rest.length() || rest == "status") {
            ConsoleFormat::print_memory_status(out, Memory::status());
            return;
        }
        if (rest == "detail") {
            ConsoleFormat::print_memory_detail_status(
                out, Memory::detail_status());
            router.print_memory_detail(out);
            return;
        }
        print_unknown_command(out, "MEM", "memory, memory detail");
        return;
    }

    if (command == "version" || command == "v") {
        trim_inplace(rest);
        if (rest.length()) {
            print_unknown_command(out, "FW", "version");
            return;
        }
        out.print("[FW] AirCANnect ");
        out.print(aircannect_version());
        out.print(" built ");
        out.println(aircannect_build_date());
        return;
    }

    if (!router.execute(command, rest, out, session_)) {
        out.println("[CLI] unknown command. Type 'help'.");
    }
}

}  // namespace aircannect
