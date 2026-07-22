#include "telnet_console.h"

#include "auth_utils.h"
#include "debug_log.h"
#include "string_print.h"
#include "version.h"

namespace aircannect {

bool TelnetConsole::begin(uint16_t port) {
    return begin_line_server(port, "TELNET");
}

bool TelnetConsole::restart(uint16_t port, ConsoleCommandRouter &router) {
    stop(router);
    return begin(port);
}

void TelnetConsole::stop(ConsoleCommandRouter &router) {
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        disconnect_slot(i, router);
    }
    stop_line_server();
}

void TelnetConsole::poll(const AppConfigData &config,
                         ConsoleCommandRouter &router) {
    if (!started()) return;
    accept_clients(config, router);

    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client || !slot.client.connected() ||
            slot.auth_state != AuthState::Authenticated ||
            !slot.console.pending_output(router)) {
            continue;
        }

        StringPrint capture(AC_FILE_LOG_TAIL_READ_CHUNK, "\r\n");
        slot.console.poll_pending(capture, router);
        if (capture.text().length()) queue_text(i, capture.text());
    }

    pump_outputs(router);
    poll_inputs(config, router);
}

void TelnetConsole::handle_event(const RpcEvent &event) {
    if (!started()) return;
    if (!ManagementConsole::event_has_output(event)) return;

    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client || !slot.client.connected() ||
            slot.auth_state != AuthState::Authenticated) {
            continue;
        }
        StringPrint capture(4096, "\r\n");
        slot.console.handle_event(capture, event);
        if (capture.text().length()) queue_text(i, capture.text());
    }
}

void TelnetConsole::accept_clients(const AppConfigData &app_config,
                                   ConsoleCommandRouter &router) {
    WiFiClient incoming = accept_line_client();
    if (!incoming) return;

    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        if (slots_[i].client && slots_[i].client.connected()) continue;
        disconnect_slot(i, router);
        slots_[i].client = incoming;
        stats_.accepted_clients++;
        Log::logf(CAT_TCP, LOG_INFO, "[TELNET %u] connected from %s\n",
                  static_cast<unsigned>(i),
                  slots_[i].client.remoteIP().toString().c_str());
        authenticate_slot(i, app_config);
        return;
    }

    stats_.rejected_clients++;
    incoming.println("ERR: max clients");
    incoming.stop();
}

void TelnetConsole::disconnect_slot(size_t idx,
                                    ConsoleCommandRouter &router) {
    if (idx >= AC_MAX_TELNET_CLIENTS) return;
    Slot &slot = slots_[idx];
    slot.console.stop(router);
    if (slot.client) slot.client.stop();
    slot.output_queue.clear();
    slot.output_current = "";
    slot.output_pos = 0;
    slot.line = "";
    slot.auth_line = "";
    slot.auth_user = "";
    slot.auth_state = AuthState::Disconnected;
    slot.telnet_skip = 0;
    slot.last_cr = false;
}

void TelnetConsole::authenticate_slot(size_t idx,
                                      const AppConfigData &app_config) {
    if (idx >= AC_MAX_TELNET_CLIENTS) return;
    Slot &slot = slots_[idx];
    slot.line = "";
    slot.auth_line = "";
    slot.auth_user = "";

    String hello = "\r\nAirCANnect ";
    hello += aircannect_version();
    hello += " management console\r\n";
    queue_text(idx, hello);

    if (network_client_allowed(app_config, slot.client.remoteIP())) {
        slot.auth_state = AuthState::Authenticated;
        stats_.auth_successes++;
        queue_console_begin(idx);
        queue_prompt(idx);
        return;
    }

    slot.auth_state = AuthState::Username;
    queue_text(idx, "login: ");
}

void TelnetConsole::queue_text(size_t idx, const String &text) {
    if (idx >= AC_MAX_TELNET_CLIENTS || !text.length()) return;
    Slot &slot = slots_[idx];
    if (!slot.output_queue.push(text)) {
        stats_.queue_drops++;
    }
}

void TelnetConsole::queue_prompt(size_t idx) {
    (void)idx;
}

void TelnetConsole::queue_console_begin(size_t idx) {
    StringPrint capture(4096, "\r\n");
    slots_[idx].console.begin(capture);
    if (capture.text().length()) queue_text(idx, capture.text());
}

void TelnetConsole::pump_outputs(ConsoleCommandRouter &router) {
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client || !slot.client.connected()) continue;

        LineOutputPumpResult result = pump_line_output(
            slot.client, slot.output_queue, slot.output_current,
            slot.output_pos, i, "TELNET", false);
        if (result.fatal_error) {
            stats_.disconnected_clients++;
            disconnect_slot(i, router);
        }
    }
}

void TelnetConsole::poll_inputs(const AppConfigData &config,
                                ConsoleCommandRouter &router) {
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client) continue;
        if (!slot.client.connected()) {
            Log::logf(CAT_TCP, LOG_INFO, "[TELNET %u] disconnected\n",
                      static_cast<unsigned>(i));
            stats_.disconnected_clients++;
            disconnect_slot(i, router);
            continue;
        }

        size_t budget = AC_TELNET_READ_BYTES_PER_POLL;
        while (budget > 0 && slot.client.available()) {
            budget--;
            char c = static_cast<char>(slot.client.read());
            stats_.bytes_in++;
            process_input_char(i, c, config, router);
        }
    }
}

void TelnetConsole::process_input_char(size_t idx,
                                       char c,
                                       const AppConfigData &config,
                                       ConsoleCommandRouter &router) {
    Slot &slot = slots_[idx];
    const uint8_t byte = static_cast<uint8_t>(c);
    if (slot.telnet_skip) {
        slot.telnet_skip--;
        return;
    }
    if (byte == 0xFF) {
        slot.telnet_skip = 2;
        return;
    }
    if (c == '\n' && slot.last_cr) {
        slot.last_cr = false;
        return;
    }
    slot.last_cr = c == '\r';

    if (slot.auth_state == AuthState::Username ||
        slot.auth_state == AuthState::Password) {
        if (c == '\r' || c == '\n') {
            process_auth_line(idx, config);
        } else if (c == '\b' || c == 0x7F) {
            if (slot.auth_line.length()) {
                slot.auth_line.remove(slot.auth_line.length() - 1);
            }
        } else if (slot.auth_line.length() < AC_TELNET_AUTH_LINE_MAX) {
            slot.auth_line += c;
        }
        return;
    }

    if (slot.auth_state != AuthState::Authenticated) return;
    if (c == '\r' || c == '\n') {
        if (slot.line.length()) execute_slot_line(idx, router);
    } else if (c == '\b' || c == 0x7F) {
        if (slot.line.length()) slot.line.remove(slot.line.length() - 1);
    } else if (slot.line.length() < AC_TCP_LINE_MAX) {
        slot.line += c;
    } else {
        stats_.overlong_lines++;
        slot.line = "";
        queue_text(idx, "\r\n[CLI] line too long; dropped\r\n");
        queue_prompt(idx);
    }
}

void TelnetConsole::process_auth_line(size_t idx,
                                      const AppConfigData &app_config) {
    Slot &slot = slots_[idx];
    String line = slot.auth_line;
    slot.auth_line = "";

    if (slot.auth_state == AuthState::Username) {
        slot.auth_user = line;
        slot.auth_state = AuthState::Password;
        queue_text(idx, "\r\npassword: ");
        return;
    }

    if (slot.auth_state != AuthState::Password) return;
    if (network_credentials_match(app_config, slot.auth_user, line)) {
        slot.auth_state = AuthState::Authenticated;
        slot.auth_user = "";
        stats_.auth_successes++;
        queue_text(idx, "\r\n");
        queue_console_begin(idx);
        queue_prompt(idx);
        return;
    }

    stats_.auth_failures++;
    slot.auth_state = AuthState::Username;
    slot.auth_user = "";
    queue_text(idx, "\r\nAuthentication failed\r\nlogin: ");
}

void TelnetConsole::execute_slot_line(size_t idx,
                                      ConsoleCommandRouter &router) {
    Slot &slot = slots_[idx];
    String line = slot.line;
    slot.line = "";
    line.trim();
    if (!line.length()) {
        queue_prompt(idx);
        return;
    }

    stats_.commands_in++;
    StringPrint capture(4096, "\r\n");
    slot.console.execute_line(line, capture, router);
    if (capture.text().length()) queue_text(idx, capture.text());
    queue_prompt(idx);
}

}  // namespace aircannect
