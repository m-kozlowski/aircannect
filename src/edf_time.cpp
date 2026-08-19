#include "edf_time.h"

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

int64_t edf_floor_epoch_ms_to_second(int64_t epoch_ms) {
    const int64_t rem = epoch_ms % 1000;
    if (rem < 0) return epoch_ms - rem - 1000;
    return epoch_ms - rem;
}

}  // namespace aircannect
