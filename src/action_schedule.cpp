#include "action_schedule.h"

#include <cstdio>

namespace aircannect {
namespace {

constexpr std::string_view DAYS[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

int weekday(std::string_view name) {
    for (int i = 0; i < 7; ++i) {
        if (name == DAYS[i]) return i;
    }
    return -1;
}

uint8_t parse_days(std::string_view list) {
    if (list == "*") return 127;
    uint8_t mask = 0;
    for (;;) {
        const auto comma = list.find(',');
        const auto item = list.substr(0, comma);
        const auto dash = item.find('-');
        int first = weekday(item.substr(0, dash));
        int last = dash == item.npos ? first : weekday(item.substr(dash + 1));
        if (first < 0 || last < 0) return 0;

        for (int day = first;; day = (day + 1) % 7) {
            mask |= 1u << day;
            if (day == last) break;
        }
        if (comma == list.npos) return mask;
        list.remove_prefix(comma + 1);
    }
}

int two_digits(std::string_view pair) {
    if (pair.size() != 2 || pair[0] < '0' || pair[0] > '9' ||
        pair[1] < '0' || pair[1] > '9') return -1;
    return (pair[0] - '0') * 10 + pair[1] - '0';
}

}  // namespace

bool parse_weekly_action(std::string_view text, WeeklyAction &out) {
    if (text.empty()) {
        out = {};
        return true;
    }

    std::string_view fields[4];
    for (auto &field : fields) {
        const auto first = text.find_first_not_of(" \t");
        if (first == text.npos) return false;
        text.remove_prefix(first);
        const auto end = text.find_first_of(" \t");
        field = text.substr(0, end);
        text.remove_prefix(field.size());
    }
    if (text.find_first_not_of(" \t") != text.npos ||
        (fields[0] != "on" && fields[0] != "off") ||
        fields[1].size() != 5 || fields[1][2] != ':') return false;

    const int hour = two_digits(fields[1].substr(0, 2));
    const int minute = two_digits(fields[1].substr(3));
    const uint8_t days = parse_days(fields[2]);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || !days) return false;

    size_t count = 0;
    const auto *actions = local_action_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        if (fields[3] != actions[i].name) continue;
        out = {fields[0] == "on", days, static_cast<uint16_t>(hour * 60 + minute),
               actions[i].id};
        return true;
    }
    return false;
}

std::string format_weekly_action(const WeeklyAction &rule) {
    if (!rule.days) return {};
    char time[16];
    snprintf(time, sizeof(time), "%s %02u:%02u ", rule.enabled ? "on" : "off",
             unsigned(rule.minute / 60), unsigned(rule.minute % 60));
    std::string value(time);
    if (rule.days == 127) {
        value += '*';
    } else {
        for (int day = 0; day < 7; ++day) {
            if (!(rule.days & (1 << day))) continue;
            if (value.back() != ' ') value += ',';
            value += DAYS[day];
        }
    }
    value += ' ';
    value += local_action_find(rule.action)->name;
    return value;
}

void ActionSchedule::configure(const WeeklyActions &rules, time_t now) {
    for (size_t i = 0; i < rules.size(); ++i) {
        if (rules_[i] == rules[i]) continue;
        rules_[i] = rules[i];
        last_date_[i] = 0;
        edited_minute_[i] = now / 60;
    }
}

uint8_t ActionSchedule::take_due(time_t now) {
    const time_t minute = now ? now / 60 : -1;
    const time_t previous = previous_minute_;
    previous_minute_ = minute;
    if (previous < 0 || minute < 0 || minute == previous) return 0;

    struct tm local{};
    if (!localtime_r(&now, &local)) return 0;
    const int date = (local.tm_year + 1900) * 1000 + local.tm_yday;
    uint8_t due = 0;
    for (size_t i = 0; i < rules_.size(); ++i) {
        const auto &rule = rules_[i];
        if (!rule.enabled || !(rule.days & (1 << local.tm_wday)) ||
            rule.minute != local.tm_hour * 60 + local.tm_min ||
            last_date_[i] == date || edited_minute_[i] == minute) continue;
        last_date_[i] = date;
        due |= 1u << i;
    }
    return due;
}

}  // namespace aircannect
