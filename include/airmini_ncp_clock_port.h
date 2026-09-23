#pragma once

#include <stdint.h>
#include <string>

namespace aircannect {

struct AirMiniNcpClockResult {
    bool succeeded = false;
    std::string datetime;
    int16_t error_code = 0;
    std::string reason;
};

class AirMiniNcpClockPort {
public:
    virtual ~AirMiniNcpClockPort() = default;

    virtual bool available() const = 0;
    virtual bool request_write(const char *datetime,
                               uint32_t now_ms) = 0;
    virtual bool take_result(AirMiniNcpClockResult &result) = 0;
    virtual bool pending() const = 0;
    virtual void cancel(const char *reason) = 0;
};

}  // namespace aircannect
