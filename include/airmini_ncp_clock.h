#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace aircannect {

enum class AirMiniNcpClockCommand : uint8_t {
    Get = 0x04,
    Set = 0x05,
};

struct AirMiniNcpClockResponse {
    uint8_t command = 0;
    uint8_t tag = 0;
    bool error = false;
    int16_t error_code = 0;
    std::string datetime;
    std::string error_text;
};

bool encode_airmini_ncp_clock_request(AirMiniNcpClockCommand command,
                                      uint8_t tag,
                                      const char *datetime,
                                      uint8_t *out,
                                      size_t capacity,
                                      size_t &written);

bool decode_airmini_ncp_clock_response(const uint8_t *data,
                                       size_t size,
                                       AirMiniNcpClockResponse &out);

}  // namespace aircannect
