#include "console_commands.h"

#include "management_console_format.h"
#include "management_console_utils.h"
#include "string_util.h"
#include "tcp_bridge.h"
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

    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (rest == "list") {
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

    if (rest == "scan") {
        print_wifi_scan(out, wifi);
        return;
    }

    if (rest == "restart" || rest == "reconnect") {
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

    if (rest == "clear") {
        out.println("[WiFi] clearing stored STA credentials");
        wifi.clear_sta_config();
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (rest.startsWith("set ")) {
        String args = rest.substring(4);
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

    if (rest.startsWith("add ")) {
        String args = rest.substring(4);
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
        const bool started = wifi.configure_open_sta(ssid);
        if (!started) {
            out.println("[WiFi] STA connect could not be started");
        } else if (!wifi.network_available()) {
            out.println("[WiFi] STA connect started; use wifi status for progress");
        }
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (rest.startsWith("remove ")) {
        String index_text = rest.substring(7);
        size_t index = 0;
        if (!parse_index_arg(index_text, wifi.profile_count(), index)) {
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

}  // namespace

NetworkConsoleCommands::NetworkConsoleCommands(WifiManager &wifi,
                                               TcpBridge &tcp)
    : wifi_(wifi), tcp_(tcp) {}

bool NetworkConsoleCommands::execute(const String &command,
                                     const String &rest_arg,
                                     Print &out,
                                     ConsoleCommandSession &session) {
    (void)session;
    if (command != "wifi" && command != "tcp") return false;

    String rest = rest_arg;

    if (command == "wifi") {
        handle_wifi(out, rest, wifi_);
        return true;
    }
    if (command == "tcp") {
        trim_inplace(rest);
        to_lower_inplace(rest);
        if (rest.length() && rest != "status") {
            print_unknown_command(out, "TCP", "tcp status");
        } else {
            ConsoleFormat::print_tcp_status(out, tcp_);
        }
        return true;
    }
    return false;
}

void NetworkConsoleCommands::print_stats(Print &out) {
    ConsoleFormat::print_tcp_status(out, tcp_);
}

}  // namespace aircannect
