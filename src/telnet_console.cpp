#include "telnet_console.h"

#include "auth_utils.h"
#include "debug_log.h"
#include "string_print.h"
#include "version.h"

namespace aircannect {

bool TelnetConsole::begin(uint16_t port) {
    return begin_line_server(port, "TELNET");
}

bool TelnetConsole::restart(uint16_t port, RpcArbiter *arbiter) {
    stop(arbiter);
    return begin(port);
}

void TelnetConsole::stop(RpcArbiter *arbiter) {
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        disconnect_slot(i, arbiter);
    }
    stop_line_server();
}

void TelnetConsole::poll(ConsoleContext &ctx) {
    if (!started()) return;
    accept_clients(ctx.app_config, ctx.arbiter);
    pump_outputs(ctx.arbiter);
    poll_inputs(ctx);
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

void TelnetConsole::print_stats(Print &out) {
    out.print(" telnet_started=");
    out.print(started() ? "yes" : "no");
    out.print(" telnet_port=");
    out.print(port());
    out.print(" telnet_clients=");
    out.print(connected_count());
    out.print(" telnet_accepted=");
    out.print(stats_.accepted_clients);
    out.print(" telnet_disconnected=");
    out.print(stats_.disconnected_clients);
    out.print(" telnet_rejected=");
    out.print(stats_.rejected_clients);
    out.print(" telnet_auth_ok=");
    out.print(stats_.auth_successes);
    out.print(" telnet_auth_fail=");
    out.print(stats_.auth_failures);
    out.print(" telnet_commands=");
    out.print(stats_.commands_in);
    out.print(" telnet_bytes_in=");
    out.print(stats_.bytes_in);
    out.print(" telnet_bytes_out=");
    out.print(line_io_stats().bytes_out);
    out.print(" telnet_write_attempts=");
    out.print(line_io_stats().write_attempts);
    out.print(" telnet_write_deferred=");
    out.print(line_io_stats().write_deferred);
    out.print(" telnet_write_errors=");
    out.print(line_io_stats().write_errors);
    out.print(" telnet_overlong=");
    out.print(stats_.overlong_lines);
    out.print(" telnet_q_drops=");
    out.print(stats_.queue_drops);
}

void TelnetConsole::print_status(Print &out) {
    out.print("[TELNET] started=");
    out.print(started() ? "yes" : "no");
    out.print(" port=");
    out.print(port());
    out.print(" clients=");
    out.println(connected_count());
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client || !slot.client.connected()) continue;
        out.print("[TELNET ");
        out.print(i);
        out.print("] remote=");
        out.print(slot.client.remoteIP());
        out.print(" state=");
        switch (slot.auth_state) {
            case AuthState::Username: out.print("username"); break;
            case AuthState::Password: out.print("password"); break;
            case AuthState::Authenticated: out.print("authenticated"); break;
            case AuthState::Disconnected:
            default: out.print("disconnected"); break;
        }
        out.print(" line_buf=");
        out.print(slot.line.length());
        out.print(" out_q=");
        out.print(slot.output_queue.count());
        out.print(" out_current=");
        out.println(slot.output_current.length());
    }
}

int TelnetConsole::connected_count() {
    int count = 0;
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        if (slots_[i].client && slots_[i].client.connected()) count++;
    }
    return count;
}

void TelnetConsole::accept_clients(const AppConfig &app_config,
                                   RpcArbiter &arbiter) {
    WiFiClient incoming = accept_line_client();
    if (!incoming) return;

    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        if (slots_[i].client && slots_[i].client.connected()) continue;
        disconnect_slot(i, &arbiter);
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

void TelnetConsole::disconnect_slot(size_t idx, RpcArbiter *arbiter) {
    if (idx >= AC_MAX_TELNET_CLIENTS) return;
    Slot &slot = slots_[idx];
    if (arbiter) slot.console.stop(*arbiter);
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
                                      const AppConfig &app_config) {
    if (idx >= AC_MAX_TELNET_CLIENTS) return;
    Slot &slot = slots_[idx];
    slot.line = "";
    slot.auth_line = "";
    slot.auth_user = "";

    String hello = "\r\nAirCANnect ";
    hello += aircannect_version();
    hello += " management console\r\n";
    queue_text(idx, hello);

    if (network_client_allowed(app_config.data(), slot.client.remoteIP())) {
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

void TelnetConsole::pump_outputs(RpcArbiter &arbiter) {
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client || !slot.client.connected()) continue;

        LineOutputPumpResult result = pump_line_output(
            slot.client, slot.output_queue, slot.output_current,
            slot.output_pos, i, "TELNET", false);
        if (result.fatal_error) {
            stats_.disconnected_clients++;
            disconnect_slot(i, &arbiter);
        }
    }
}

void TelnetConsole::poll_inputs(ConsoleContext &ctx) {
    for (size_t i = 0; i < AC_MAX_TELNET_CLIENTS; ++i) {
        Slot &slot = slots_[i];
        if (!slot.client) continue;
        if (!slot.client.connected()) {
            Log::logf(CAT_TCP, LOG_INFO, "[TELNET %u] disconnected\n",
                      static_cast<unsigned>(i));
            stats_.disconnected_clients++;
            disconnect_slot(i, &ctx.arbiter);
            continue;
        }

        size_t budget = AC_TELNET_READ_BYTES_PER_POLL;
        while (budget > 0 && slot.client.available()) {
            budget--;
            char c = static_cast<char>(slot.client.read());
            stats_.bytes_in++;
            process_input_char(i, c, ctx);
        }
    }
}

void TelnetConsole::process_input_char(size_t idx,
                                       char c,
                                       ConsoleContext &ctx) {
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
            process_auth_line(idx, ctx.app_config);
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
        if (slot.line.length()) execute_slot_line(idx, ctx);
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
                                      const AppConfig &app_config) {
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
    if (network_credentials_match(app_config.data(), slot.auth_user, line)) {
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

void TelnetConsole::execute_slot_line(size_t idx, ConsoleContext &ctx) {
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
    slot.console.execute_line(line, capture, ctx);
    if (capture.text().length()) queue_text(idx, capture.text());
    queue_prompt(idx);
}

}  // namespace aircannect
