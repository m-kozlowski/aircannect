#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stdint.h>

#include "app_config.h"
#include "board.h"
#include "fixed_queue.h"
#include "line_protocol_server.h"
#include "management_console.h"
#include "ota_manager.h"
#include "resmed_ota_manager.h"
#include "rpc_transport_ports.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "tcp_bridge.h"
#include "time_sync_service.h"
#include "wifi_manager.h"

namespace aircannect {

struct TelnetConsoleStats {
    uint32_t accepted_clients = 0;
    uint32_t disconnected_clients = 0;
    uint32_t rejected_clients = 0;
    uint32_t auth_successes = 0;
    uint32_t auth_failures = 0;
    uint32_t commands_in = 0;
    uint32_t bytes_in = 0;
    uint32_t overlong_lines = 0;
    uint32_t queue_drops = 0;
};

class TelnetConsole : private LineProtocolServerBase {
public:
    bool begin(uint16_t port = AC_TELNET_CONSOLE_PORT);
    bool restart(uint16_t port = AC_TELNET_CONSOLE_PORT,
                 StreamBroker *stream = nullptr);
    void stop(StreamBroker *stream = nullptr);
    void poll(ConsoleContext &ctx);

    void handle_event(const RpcEvent &event);

    int connected_count();
    bool started() const { return line_server_started(); }
    uint16_t port() const { return line_server_port(); }

private:
    enum class AuthState {
        Disconnected,
        Username,
        Password,
        Authenticated,
    };

    struct Slot {
        WiFiClient client;
        ManagementConsole console;
        FixedQueue<String, AC_TELNET_TX_QUEUE_DEPTH> output_queue;
        String output_current;
        String line;
        String auth_line;
        String auth_user;
        size_t output_pos = 0;
        AuthState auth_state = AuthState::Disconnected;
        uint8_t telnet_skip = 0;
        bool last_cr = false;
    };

    void accept_clients(const AppConfigData &app_config,
                        StreamBroker &stream);
    void disconnect_slot(size_t idx, StreamBroker *stream = nullptr);
    void authenticate_slot(size_t idx, const AppConfigData &app_config);

    void queue_text(size_t idx, const String &text);
    void queue_prompt(size_t idx);
    void queue_console_begin(size_t idx);
    void pump_outputs(StreamBroker &stream);

    void poll_inputs(ConsoleContext &ctx);
    void process_input_char(size_t idx,
                            char c,
                            ConsoleContext &ctx);
    void process_auth_line(size_t idx, const AppConfigData &app_config);
    void execute_slot_line(size_t idx,
                           ConsoleContext &ctx);

    Slot slots_[AC_MAX_TELNET_CLIENTS];
    TelnetConsoleStats stats_ = {};
};

}  // namespace aircannect
