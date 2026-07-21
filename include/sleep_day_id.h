#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class SleepDayId {
public:
    static constexpr int32_t InvalidEpochDay = INT32_MIN;

    constexpr SleepDayId() = default;

    static bool from_yyyymmdd(const char *text, SleepDayId &out);
    static bool from_epoch_days(int64_t epoch_days, SleepDayId &out);

    constexpr bool valid() const {
        return epoch_day_ != InvalidEpochDay;
    }
    constexpr int32_t epoch_days() const { return epoch_day_; }

    bool format_yyyymmdd(char *out, size_t out_size) const;

    friend constexpr bool operator==(SleepDayId lhs, SleepDayId rhs) {
        return lhs.epoch_day_ == rhs.epoch_day_;
    }
    friend constexpr bool operator!=(SleepDayId lhs, SleepDayId rhs) {
        return !(lhs == rhs);
    }
    friend constexpr bool operator<(SleepDayId lhs, SleepDayId rhs) {
        return lhs.epoch_day_ < rhs.epoch_day_;
    }

private:
    explicit constexpr SleepDayId(int32_t epoch_day) :
        epoch_day_(epoch_day) {}

    int32_t epoch_day_ = InvalidEpochDay;
};

}  // namespace aircannect
