#pragma once

#include <array>
#include <ctime>
#include <string>
#include <string_view>

#include "local_input.h"

namespace aircannect {

struct ScheduledAction {
    bool enabled = false;
    uint8_t days = 0;  // Weekly rules: bit 0 = Sunday
    uint16_t minute = 0;
    LocalActionId action = LocalActionId::None;
    uint32_t interval_s = 0;  // Interval rules have no calendar fields

    bool operator==(const ScheduledAction &other) const {
        return enabled == other.enabled && days == other.days &&
               minute == other.minute && action == other.action &&
               interval_s == other.interval_s;
    }
};

using ScheduledActions = std::array<ScheduledAction, 4>;
bool parse_scheduled_action(std::string_view text, ScheduledAction &out);
std::string format_scheduled_action(const ScheduledAction &rule);

class ActionSchedule {
public:
    void configure(const ScheduledActions &rules, time_t wall, uint64_t uptime_ms);
    // A zero wall clock disables only calendar rules. Due slots are consumed.
    uint8_t take_due(time_t wall, uint64_t uptime_ms);
    bool should_log_result(size_t slot, const char *error);

private:
    ScheduledActions rules_{};
    struct SlotState {
        int last_date = 0;
        time_t edited_minute = 0;
        uint64_t next_ms = 0;
        const char *last_error = nullptr;
    };
    std::array<SlotState, ScheduledActions{}.size()> slots_{};
    time_t previous_minute_ = -1;
};

}  // namespace aircannect
