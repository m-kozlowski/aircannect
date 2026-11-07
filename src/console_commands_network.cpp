#include "console_commands.h"

#include "management_console_format.h"
#include "management_console_utils.h"
#include "string_util.h"
#include "wifi_manager.h"

namespace aircannect {
namespace {

void print_wifi_scan(Print &out, WifiManager &wifi) {
    switch (wifi.manual_scan_status()) {
        case WifiScanStatus::RoamInProgress:
            out.println("[WiFi] roaming scan in progress; try again shortly");
            return;
        case WifiScanStatus::Running:
            out.println("[WiFi] scan running; run wifi scan again for results");
            return;
        case WifiScanStatus::Failed:
            out.println("[WiFi] scan failed");
            wifi.clear_manual_scan_results();
            return;
        case WifiScanStatus::Ready: {
            static constexpr size_t MAX_RESULTS = 32;
            WifiScanNetwork results[MAX_RESULTS];
            const size_t count =
                wifi.copy_manual_scan_results(results, MAX_RESULTS);
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
            wifi.clear_manual_scan_results();
            return;
        }
        case WifiScanStatus::Idle:
            break;
    }

    switch (wifi.start_manual_scan()) {
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

void handle_wifi(Print &out, String rest, WifiManager &wifi) {
    trim_inplace(rest);

    int position = 0;
    String action;
    if (!parse_console_arg(rest, position, action)) action = "status";
    action.toLowerCase();

    String args = rest.substring(position);
    trim_inplace(args);
    if (action == "status" && !args.length()) {
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (action == "list" && !args.length()) {
        out.print("[WiFi profiles] count=");
        out.print(wifi.profile_count());
        out.print(" active=");
        const int8_t active = wifi.active_profile_index();
        if (active >= 0) {
            out.println(active);
        } else {
            out.println("none");
        }

        for (size_t i = 0; i < wifi.profile_count(); ++i) {
            const WifiProfile &profile = wifi.profile(i);
            out.print("  ");
            out.print(i);
            out.print(": ssid=\"");
            out.print(profile.ssid);
            out.print("\" auth=");
            out.print(profile.password.length() ? "password" : "open");
            if (active == static_cast<int8_t>(i)) out.print(" active=yes");
            out.println();
        }
        return;
    }

    if (action == "scan" && !args.length()) {
        print_wifi_scan(out, wifi);
        return;
    }

    if ((action == "restart" || action == "reconnect") && !args.length()) {
        out.println("[WiFi] reconnecting...");
        const bool started = wifi.reconnect();
        if (!started) {
            out.println("[WiFi] no STA credentials and SoftAP fallback is off");
        } else if (!wifi.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (action == "clear" && !args.length()) {
        out.println("[WiFi] clearing stored STA credentials");
        wifi.clear_sta_config();
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (action == "set") {
        int pos = 0;
        String ssid;
        String password;
        if (!parse_console_arg(args, pos, ssid) ||
            !parse_console_arg(args, pos, password) || !ssid.length()) {
            out.println("[WiFi] usage: wifi set SSID PASSWORD");
            out.println("[WiFi] quote SSID/password when they contain spaces");
            return;
        }

        out.print("[WiFi] saving STA credentials for SSID=\"");
        out.print(ssid);
        out.println("\"");
        const bool started = wifi.configure_sta(ssid, password);
        if (!started) {
            out.println("[WiFi] STA connect could not be started");
        } else if (!wifi.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (action == "add") {
        int pos = 0;
        String ssid;
        String password;
        if (!parse_console_arg(args, pos, ssid) ||
            !parse_console_arg(args, pos, password) || !ssid.length()) {
            out.println("[WiFi] usage: wifi add SSID PASSWORD");
            out.println("[WiFi] quote SSID/password when they contain spaces");
            return;
        }

        out.print("[WiFi] adding STA profile SSID=\"");
        out.print(ssid);
        out.println("\"");
        if (!wifi.add_profile(ssid, password, false)) {
            out.println("[WiFi] profile add failed");
        }
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (action == "open") {
        int pos = 0;
        String ssid;
        if (!parse_console_arg(args, pos, ssid) || !ssid.length()) {
            out.println("[WiFi] usage: wifi open SSID");
            return;
        }

        out.print("[WiFi] saving open STA network SSID=\"");
        out.print(ssid);
        out.println("\"");
        const bool started = wifi.configure_open_sta(ssid);
        if (!started) {
            out.println("[WiFi] STA connect could not be started");
        } else if (!wifi.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (action == "remove") {
        size_t index = 0;
        if (!parse_index_arg(args, wifi.profile_count(), index)) {
            out.println("[WiFi] usage: wifi remove INDEX");
            return;
        }
        if (!wifi.remove_profile(index)) {
            out.println("[WiFi] profile remove failed");
            return;
        }
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    print_unknown_command(out, "WiFi",
                          "wifi status, list, scan, set, add, open, remove, "
                          "clear, restart");
}

bool poll_ping(Print &out, PingDiagnostic &ping, uint32_t elapsed_ms) {
    ping.poll();
    if (!ping.finished()) {
        if (!ping.resolving() || elapsed_ms < PingDiagnostic::ResolveTimeoutMs) {
            return false;
        }
        out.println("[PING] DNS timed out; resolver will finish in the background");
        ping.cancel();
        return true;
    }

    const auto &status = ping.result();
    if (status.error) {
        out.printf("[PING] failed: %s\n", status.error);
    } else if (status.sent == 0) {
        out.println("[PING] no packets sent; check network connectivity");
    } else {
        char address[16] = {};
        ipaddr_ntoa_r(&status.address, address, sizeof(address));
        out.printf("[PING] target=%s sent=%lu received=%lu loss=%lu%%",
                   address, static_cast<unsigned long>(status.sent),
                   static_cast<unsigned long>(status.received),
                   static_cast<unsigned long>(
                       (status.sent - status.received) * 100 / status.sent));
        if (status.received) {
            out.printf(" rtt_min/avg/max=%lu/%lu/%lums",
                       static_cast<unsigned long>(status.min_ms),
                       static_cast<unsigned long>(status.sum_ms / status.received),
                       static_cast<unsigned long>(status.max_ms));
        }
        out.println();
    }
    return true;
}

void handle_ping(Print &out, const String &rest, PingDiagnostic &ping,
                 const ConsoleCommandSession &session,
                 NetworkPendingCommand &pending_command,
                 uint32_t &pending_session_id, uint32_t &started_ms) {
    int position = 0;
    String host, count_text, extra;
    uint16_t count = PingDiagnostic::DefaultCount;
    bool valid = parse_console_arg(rest, position, host) && host.length();
    const bool has_count = parse_console_arg(rest, position, count_text);
    if (!has_count && count_text.length()) valid = false;
    if (has_count && !parse_uint16_arg(count_text, count)) valid = false;
    if (parse_console_arg(rest, position, extra) || extra.length() ||
        host.length() > 253 || count == 0 || count > PingDiagnostic::MaxCount) {
        valid = false;
    }
    if (!valid) {
        out.println("[PING] usage: ping HOST [COUNT] (IPv4; default 4, range 1-10)");
        return;
    }

    if (pending_command != NetworkPendingCommand::None ||
        !ping.start(host.c_str(), count)) {
        out.println("[PING] another probe is running or finishing; try again shortly");
        return;
    }
    pending_command = NetworkPendingCommand::Ping;
    pending_session_id = session.id;
    started_ms = millis();
    out.printf("[PING] resolving %s; count=%u timeout=1000ms interval=1000ms\n",
               host.c_str(), unsigned(count));
}

}  // namespace

NetworkConsoleCommands::NetworkConsoleCommands(WifiManager &wifi)
    : wifi_(wifi) {}

bool NetworkConsoleCommands::execute(const String &command,
                                     const String &rest_arg,
                                     Print &out,
                                     ConsoleCommandSession &session) {
    String rest = rest_arg;

    if (command == "wifi") {
        handle_wifi(out, rest, wifi_);
        return true;
    }
    if (command == "ping") {
        handle_ping(out, rest, ping_, session, pending_command_,
                    pending_session_id_, pending_started_ms_);
        return true;
    }
    return false;
}

void NetworkConsoleCommands::poll_pending(Print &out,
                                          ConsoleCommandSession &session) {
    if (!pending_output(session)) return;
    bool complete = false;
    switch (pending_command_) {
        case NetworkPendingCommand::Ping:
            complete = poll_ping(out, ping_, millis() - pending_started_ms_);
            break;
        case NetworkPendingCommand::None:
            return;
    }
    if (complete) {
        pending_command_ = NetworkPendingCommand::None;
        pending_session_id_ = 0;
        pending_started_ms_ = 0;
    }
}

bool NetworkConsoleCommands::pending_output(const ConsoleCommandSession &session) const {
    return pending_command_ != NetworkPendingCommand::None &&
        pending_session_id_ != 0 && pending_session_id_ == session.id;
}

void NetworkConsoleCommands::cancel_pending(ConsoleCommandSession &session) {
    if (!pending_output(session)) return;
    switch (pending_command_) {
        case NetworkPendingCommand::Ping:
            ping_.cancel();
            break;
        case NetworkPendingCommand::None:
            break;
    }
    pending_command_ = NetworkPendingCommand::None;
    pending_session_id_ = 0;
    pending_started_ms_ = 0;
}

void NetworkConsoleCommands::stop(ConsoleCommandSession &session) {
    cancel_pending(session);
}

}  // namespace aircannect
