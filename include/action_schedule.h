#pragma once

#include <array>
#include <ctime>
#include <string>
#include <string_view>

#include "local_input.h"

namespace aircannect {

struct WeeklyAction {
    bool enabled = false;
    uint8_t days = 0;  // bit 0 = Sunday; zero = empty slot
    uint16_t minute = 0;
    LocalActionId action = LocalActionId::None;

    bool operator==(const WeeklyAction &other) const {
        return enabled == other.enabled && days == other.days &&
               minute == other.minute && action == other.action;
    }
};

using WeeklyActions = std::array<WeeklyAction, 4>;
bool parse_weekly_action(std::string_view text, WeeklyAction &out);
std::string format_weekly_action(const WeeklyAction &rule);

class ActionSchedule {
public:
    void configure(const WeeklyActions &rules, time_t now);
    // Zero is an invalid clock. Returned slots are consumed even on rejection.
    uint8_t take_due(time_t now);

private:
    WeeklyActions rules_{};
    std::array<int, WeeklyActions{}.size()> last_date_{};
    std::array<time_t, WeeklyActions{}.size()> edited_minute_{};
    time_t previous_minute_ = -1;
};

}  // namespace aircannect
