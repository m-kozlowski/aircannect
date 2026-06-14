#pragma once

#include <string.h>

namespace aircannect {

inline bool session_utc_timestamp_later(const char *candidate,
                                        const char *current) {
    if (!candidate || !candidate[0]) return false;
    if (!current || !current[0]) return true;
    return strcmp(candidate, current) > 0;
}

}  // namespace aircannect
