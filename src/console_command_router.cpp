#include "console_command_router.h"

namespace aircannect {

bool ConsoleCommandRouter::begin(ConsoleCommandGroup *const *groups,
                                 size_t group_count) {
    if (!groups || !group_count) return false;

    for (size_t i = 0; i < group_count; ++i) {
        if (!groups[i]) return false;
    }

    groups_ = groups;
    group_count_ = group_count;
    return true;
}

bool ConsoleCommandRouter::execute(const String &command,
                                   const String &rest,
                                   Print &out,
                                   ConsoleCommandSession &session) {
    if (!groups_) return false;
    ensure_session(session);

    for (size_t i = 0; i < group_count_; ++i) {
        if (groups_[i]->execute(command, rest, out, session)) return true;
    }
    return false;
}

void ConsoleCommandRouter::poll_pending(Print &out,
                                        ConsoleCommandSession &session) {
    if (!groups_ || !session.id) return;

    for (size_t i = 0; i < group_count_; ++i) {
        groups_[i]->poll_pending(out, session);
    }
}

bool ConsoleCommandRouter::pending_output(
    const ConsoleCommandSession &session) const {
    if (!groups_ || !session.id) return false;

    for (size_t i = 0; i < group_count_; ++i) {
        if (groups_[i]->pending_output(session)) return true;
    }
    return false;
}

void ConsoleCommandRouter::cancel_pending(ConsoleCommandSession &session) {
    if (!groups_ || !session.id) return;

    for (size_t i = 0; i < group_count_; ++i) {
        groups_[i]->cancel_pending(session);
    }
}

void ConsoleCommandRouter::stop(ConsoleCommandSession &session) {
    if (groups_ && session.id) {
        for (size_t i = 0; i < group_count_; ++i) {
            groups_[i]->stop(session);
        }
    }
    session = {};
}

void ConsoleCommandRouter::ensure_session(ConsoleCommandSession &session) {
    if (session.id) return;

    ++next_session_id_;
    if (!next_session_id_) ++next_session_id_;
    session.id = next_session_id_;
}

void ConsoleCommandRouter::print_status(Print &out) {
    if (!groups_) return;

    for (size_t i = 0; i < group_count_; ++i) {
        groups_[i]->print_status(out);
    }
}

void ConsoleCommandRouter::print_stats(Print &out) {
    if (!groups_) return;

    for (size_t i = 0; i < group_count_; ++i) {
        groups_[i]->print_stats(out);
    }
}

void ConsoleCommandRouter::reset_stats() {
    if (!groups_) return;

    for (size_t i = 0; i < group_count_; ++i) {
        groups_[i]->reset_stats();
    }
}

void ConsoleCommandRouter::print_memory_detail(Print &out) {
    if (!groups_) return;

    for (size_t i = 0; i < group_count_; ++i) {
        groups_[i]->print_memory_detail(out);
    }
}

}  // namespace aircannect
