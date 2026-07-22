#pragma once

#include <stdint.h>

namespace aircannect {

class RuntimeClockPort {
public:
    virtual ~RuntimeClockPort() = default;

    virtual uint32_t monotonic_ms() const = 0;
    virtual int64_t utc_ms() const = 0;
};

}  // namespace aircannect
