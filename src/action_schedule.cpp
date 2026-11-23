#include "action_schedule.h"

#include <charconv>
#include <cstdio>
#include <cstring>

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

uint32_t parse_interval(std::string_view token) {
    if (token.size() < 2) return 0;
    const char unit = token.back();
    const unsigned scale = unit == 's' ? 1 : unit == 'm' ? 60 : unit == 'h' ? 3600 : 0;
    if (!scale) return 0;

    token.remove_suffix(1);
    uint32_t amount = 0;
    const char *end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data(), end, amount);
    if (parsed.ec != std::errc{} || parsed.ptr != end || amount > UINT32_MAX / scale) return 0;
    return amount * scale;
}

}  // namespace

bool parse_scheduled_action(std::string_view text, ScheduledAction &out) {
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
        (fields[0] != "on" && fields[0] != "off")) return false;

    ScheduledAction parsed;
    parsed.enabled = fields[0] == "on";
    if (fields[1] == "every") {
        parsed.interval_s = parse_interval(fields[2]);
        if (!parsed.interval_s) return false;
    } else {
        if (fields[1].size() != 5 || fields[1][2] != ':') return false;
        const int hour = two_digits(fields[1].substr(0, 2));
        const int minute = two_digits(fields[1].substr(3));
        parsed.days = parse_days(fields[2]);
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || !parsed.days) return false;
        parsed.minute = static_cast<uint16_t>(hour * 60 + minute);
    }

    size_t count = 0;
    const auto *actions = local_action_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        if (fields[3] != actions[i].name) continue;
        parsed.action = actions[i].id;
        out = parsed;
        return true;
    }
    return false;
}

std::string format_scheduled_action(const ScheduledAction &rule) {
    if (!rule.days && !rule.interval_s) return {};
    std::string value(rule.enabled ? "on " : "off ");
    if (rule.interval_s) {
        const unsigned scale = rule.interval_s % 3600 == 0 ? 3600 : rule.interval_s % 60 == 0 ? 60 : 1;
        char interval[24];
        snprintf(interval, sizeof(interval), "every %u%c", unsigned(rule.interval_s / scale),
                 scale == 3600 ? 'h' : scale == 60 ? 'm' : 's');
        value += interval;
    } else {
        char clock[8];
        snprintf(clock, sizeof(clock), "%02u:%02u ", unsigned(rule.minute / 60),
                 unsigned(rule.minute % 60));
        value += clock;

        if (rule.days == 127) {
            value += '*';
        } else {
            for (int day = 0; day < 7; ++day) {
                if (!(rule.days & (1 << day))) continue;
                if (value.back() != ' ') value += ',';
                value += DAYS[day];
            }
        }
    }
    value += ' ';
    value += local_action_find(rule.action)->name;
    return value;
}

void ActionSchedule::configure(const ScheduledActions &rules, time_t now, uint64_t uptime_ms) {
    for (size_t i = 0; i < rules.size(); ++i) {
        if (rules_[i] == rules[i]) continue;
        rules_[i] = rules[i];
        slots_[i] = {};
        slots_[i].edited_minute = now / 60;
        slots_[i].next_ms = uptime_ms + uint64_t(rules[i].interval_s) * 1000;
    }
}

uint8_t ActionSchedule::take_due(time_t now, uint64_t uptime_ms) {
    const time_t minute = now ? now / 60 : -1;
    const time_t previous = previous_minute_;
    previous_minute_ = minute;
    struct tm local{};
    const bool entered_minute = previous >= 0 && minute >= 0 && minute != previous &&
                                localtime_r(&now, &local);
    const int date = (local.tm_year + 1900) * 1000 + local.tm_yday;
    uint8_t due = 0;
    for (size_t i = 0; i < rules_.size(); ++i) {
        const auto &rule = rules_[i];
        if (!rule.enabled) continue;
        auto &slot = slots_[i];
        if (rule.interval_s) {
            if (uptime_ms < slot.next_ms) continue;
            // Advance on the original cadence; missed ticks never form a backlog.
            const uint64_t period = uint64_t(rule.interval_s) * 1000;
            slot.next_ms = uptime_ms + period - (uptime_ms - slot.next_ms) % period;
        } else {
            if (!entered_minute || !(rule.days & (1 << local.tm_wday)) ||
                rule.minute != local.tm_hour * 60 + local.tm_min ||
                slot.last_date == date || slot.edited_minute == minute) continue;
            slot.last_date = date;
        }
        due |= 1u << i;
    }
    return due;
}

bool ActionSchedule::should_log_result(size_t index, const char *error) {
    if (!rules_[index].interval_s) return true;
    const char *previous = slots_[index].last_error;
    slots_[index].last_error = error;
    return !previous || !error || strcmp(previous, error) != 0;
}

}  // namespace aircannect
