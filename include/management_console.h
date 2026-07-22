#pragma once

#include <Arduino.h>

#include "console_command_router.h"

namespace aircannect {

struct RpcEvent;

class ManagementConsole {
public:
    void begin(Print &out);
    void stop(ConsoleCommandRouter &router);
    void poll(Stream &input, Print &out, ConsoleCommandRouter &router);
    void poll_pending(Print &out, ConsoleCommandRouter &router);
    void cancel_pending(ConsoleCommandRouter &router);
    bool pending_output(const ConsoleCommandRouter &router) const {
        return router.pending_output(session_);
    }
    void execute_line(String line, Print &out, ConsoleCommandRouter &router);

    void handle_event(Print &out, const RpcEvent &event);
    void print_help(Print &out, const String &topic = "");
    static bool event_has_output(const RpcEvent &event);

private:
    String line_;
    ConsoleCommandSession session_;
};

}  // namespace aircannect
