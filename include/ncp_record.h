#pragma once

#include <stdint.h>
#include <string>
#include <string_view>

namespace aircannect {

struct NcpRecordView {
    uint8_t command = 0;
    uint8_t tag = 0;
    std::string_view payload;
};

// Views borrow the complete reassembled datagram; no command semantics here.
bool decode_ncp_record(std::string_view bytes, NcpRecordView &out);
bool encode_ncp_record(uint8_t command, uint8_t tag,
                       std::string_view payload, std::string &out);

struct NcpErrorView {
    int16_t code = 0;
    std::string_view message;
    std::string_view detail;
};

bool decode_ncp_error(std::string_view payload, NcpErrorView &out);

}  // namespace aircannect
