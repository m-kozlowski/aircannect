#include "edf_time.h"

#include <time.h>

#include "calendar_utils.h"
#include "utc_time.h"

namespace aircannect {
bool edf_parse_utc_ms(const char *text, int64_t &epoch_ms) {
    return parse_utc_iso8601_ms(text, epoch_ms);
}

bool edf_parse_as11_utc_ms(const char *text,
                           const As11ClockTransform &transform,
                           int64_t &epoch_ms) {
    int64_t device_epoch_ms = 0;
    if (!edf_parse_utc_ms(text, device_epoch_ms)) return false;
    return transform.to_utc_ms(device_epoch_ms, epoch_ms);
}

bool edf_configured_timezone_offset_minutes(int64_t epoch_ms,
                                            int32_t &out) {
    if (epoch_ms <= 0) return false;

    const int64_t seconds64 = epoch_ms / 1000LL;
    const time_t seconds = static_cast<time_t>(seconds64);
    if (static_cast<int64_t>(seconds) != seconds64) return false;

    struct tm local = {};
    if (!localtime_r(&seconds, &local)) return false;

    const int64_t local_seconds =
        calendar_days_from_civil(local.tm_year + 1900,
                                 static_cast<unsigned>(local.tm_mon + 1),
                                 static_cast<unsigned>(local.tm_mday)) *
            86400LL +
        local.tm_hour * 3600LL + local.tm_min * 60LL + local.tm_sec;
    const int64_t offset_seconds = local_seconds - seconds64;
    if (offset_seconds % 60LL != 0 ||
        offset_seconds < -24LL * 60LL * 60LL ||
        offset_seconds > 24LL * 60LL * 60LL) {
        return false;
    }

    out = static_cast<int32_t>(offset_seconds / 60LL);
    return true;
}

int64_t edf_floor_epoch_ms_to_second(int64_t epoch_ms) {
    const int64_t rem = epoch_ms % 1000;
    if (rem < 0) return epoch_ms - rem - 1000;
    return epoch_ms - rem;
}

}  // namespace aircannect
