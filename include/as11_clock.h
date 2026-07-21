#pragma once

#include <stdint.h>

namespace aircannect {

struct As11ClockTransform {
    int64_t device_minus_utc_ms = 0;
    uint32_t sampled_ms = 0;
    bool externally_referenced = false;

    bool to_utc_ms(int64_t device_epoch_ms, int64_t &utc_epoch_ms) const {
        utc_epoch_ms = device_epoch_ms;
        if (!externally_referenced) return true;

        if (device_minus_utc_ms > 0 &&
            device_epoch_ms < INT64_MIN + device_minus_utc_ms) {
            return false;
        }
        if (device_minus_utc_ms < 0 &&
            device_epoch_ms > INT64_MAX + device_minus_utc_ms) {
            return false;
        }

        utc_epoch_ms = device_epoch_ms - device_minus_utc_ms;
        return true;
    }
};

}  // namespace aircannect
