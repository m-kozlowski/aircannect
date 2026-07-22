#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct ConsoleCommandSession {
    uint32_t id = 0;
};

class ConsoleCommandGroup {
public:
    virtual ~ConsoleCommandGroup() = default;

    virtual bool execute(const String &command,
                         const String &rest,
                         Print &out,
                         ConsoleCommandSession &session) = 0;

    virtual void poll_pending(Print &out, ConsoleCommandSession &session) {
        (void)out;
        (void)session;
    }
    virtual bool pending_output(const ConsoleCommandSession &session) const {
        (void)session;
        return false;
    }
    virtual void cancel_pending(ConsoleCommandSession &session) {
        (void)session;
    }
    virtual void stop(ConsoleCommandSession &session) {
        (void)session;
    }

    virtual void print_status(Print &out) {
        (void)out;
    }
    virtual void print_stats(Print &out) {
        (void)out;
    }
    virtual void reset_stats() {}
    virtual void print_memory_detail(Print &out) {
        (void)out;
    }
};

class ConsoleCommandRouter {
public:
    bool begin(ConsoleCommandGroup *const *groups, size_t group_count);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session);
    void poll_pending(Print &out, ConsoleCommandSession &session);
    bool pending_output(const ConsoleCommandSession &session) const;
    void cancel_pending(ConsoleCommandSession &session);
    void stop(ConsoleCommandSession &session);

    void print_status(Print &out);
    void print_stats(Print &out);
    void reset_stats();
    void print_memory_detail(Print &out);

private:
    void ensure_session(ConsoleCommandSession &session);

    ConsoleCommandGroup *const *groups_ = nullptr;
    size_t group_count_ = 0;
    uint32_t next_session_id_ = 0;
};

}  // namespace aircannect
