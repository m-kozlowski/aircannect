#pragma once

#include <stddef.h>
#include <stdint.h>

#include "operation_outcome.h"
#include "sleep_day_id.h"

namespace aircannect {

// A durable source publication invalidates the report materialized for this
// canonical sleep-day range. ReportTask stamps revision when it accepts the
// notice; producer generations are not comparable across producers.
struct ReportSourceChange {
    SleepDayId first_day;
    SleepDayId last_day;
    uint32_t revision = 0;

    bool valid() const {
        return first_day.valid() && last_day.valid() &&
               !(last_day < first_day);
    }

    bool overlaps_or_touches(const ReportSourceChange &other) const {
        if (!valid() || !other.valid()) return false;
        const int64_t left_end = static_cast<int64_t>(last_day.epoch_days());
        const int64_t right_start =
            static_cast<int64_t>(other.first_day.epoch_days());
        const int64_t other_end =
            static_cast<int64_t>(other.last_day.epoch_days());
        const int64_t this_start =
            static_cast<int64_t>(first_day.epoch_days());
        return left_end + 1 >= right_start && other_end + 1 >= this_start;
    }

    void merge(const ReportSourceChange &other) {
        if (!other.valid()) return;
        if (!valid()) {
            *this = other;
            return;
        }
        if (other.first_day < first_day) first_day = other.first_day;
        if (last_day < other.last_day) last_day = other.last_day;
        if (revision < other.revision) revision = other.revision;
    }
};

static constexpr size_t AC_REPORT_SOURCE_CHANGE_CAPACITY = 8;

inline void merge_report_source_change(ReportSourceChange *entries,
                                       size_t &count,
                                       const ReportSourceChange &change) {
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].overlaps_or_touches(change)) continue;

        entries[i].merge(change);
        return;
    }

    if (count < AC_REPORT_SOURCE_CHANGE_CAPACITY) {
        entries[count++] = change;
        return;
    }

    ReportSourceChange merged = change;
    for (size_t i = 0; i < count; ++i) merged.merge(entries[i]);
    entries[0] = merged;
    count = 1;
}

using ReportSourceChangeCallback = OperationAdmission (*) (
    void *context, const ReportSourceChange &change);

}  // namespace aircannect
