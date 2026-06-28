#pragma once

#include <stddef.h>
#include <string.h>

namespace aircannect {

inline bool storage_export_datalog_day_finalized(const char *day,
                                                 const char *latest_day) {
    if (!day || !latest_day || !latest_day[0] || strlen(day) != 8) {
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (day[i] < '0' || day[i] > '9') return false;
    }
    return strcmp(day, latest_day) < 0;
}

}  // namespace aircannect
