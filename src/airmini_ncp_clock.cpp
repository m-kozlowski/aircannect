#include "airmini_ncp_clock.h"

#include <string.h>

namespace aircannect {
namespace {

constexpr size_t NcpHeaderBytes = 4;
constexpr size_t DateBytes = 24;

uint16_t read_u16_le(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

bool valid_datetime(const uint8_t *data) {
    static constexpr char separators[DateBytes] = {
        0, 0, 0, 0, '-', 0, 0, '-', 0, 0, 'T', 0,
        0, ':', 0, 0, ':', 0, 0, '.', 0, 0, 0, 'Z',
    };
    for (size_t i = 0; i < DateBytes; ++i) {
        if (separators[i]) {
            if (data[i] != static_cast<uint8_t>(separators[i])) return false;
            continue;
        }
        if (data[i] < '0' || data[i] > '9') return false;
    }
    return true;
}

bool read_length_prefixed_text(const uint8_t *data,
                               size_t size,
                               size_t &offset,
                               std::string &out) {
    if (offset > size || size - offset < 2) return false;

    const size_t length = read_u16_le(data + offset);
    offset += 2;
    if (length > size - offset) return false;

    out.assign(reinterpret_cast<const char *>(data + offset), length);
    offset += length;
    return true;
}

}  // namespace

bool encode_airmini_ncp_clock_request(AirMiniNcpClockCommand command,
                                      uint8_t tag,
                                      const char *datetime,
                                      uint8_t *out,
                                      size_t capacity,
                                      size_t &written) {
    written = 0;
    if (!out || tag == 0xff) return false;

    if (command != AirMiniNcpClockCommand::Get &&
        command != AirMiniNcpClockCommand::Set) {
        return false;
    }

    const bool set = command == AirMiniNcpClockCommand::Set;
    if (set && (!datetime || strlen(datetime) != DateBytes ||
                !valid_datetime(reinterpret_cast<const uint8_t *>(datetime)))) {
        return false;
    }

    const size_t payload_size = set ? DateBytes : 0;
    const size_t record_size = NcpHeaderBytes + payload_size;
    if (capacity < record_size) return false;

    const uint16_t length = static_cast<uint16_t>(2 + payload_size);
    out[0] = static_cast<uint8_t>(length & 0xff);
    out[1] = static_cast<uint8_t>(length >> 8);
    out[2] = static_cast<uint8_t>(command);
    out[3] = tag;
    if (set) memcpy(out + NcpHeaderBytes, datetime, DateBytes);
    written = record_size;
    return true;
}

bool decode_airmini_ncp_clock_response(const uint8_t *data,
                                       size_t size,
                                       AirMiniNcpClockResponse &out) {
    out = {};
    if (!data || size < NcpHeaderBytes) return false;

    const size_t record_length = read_u16_le(data);
    if (record_length != size - 2) return false;

    out.command = data[2];
    out.tag = data[3];
    if (out.command == 0xfd) {
        if (size < 6) return false;

        out.error = true;
        out.error_code = static_cast<int16_t>(read_u16_le(data + 4));
        size_t offset = 6;
        if (!read_length_prefixed_text(data, size, offset, out.error_text)) {
            return false;
        }

        std::string detail;
        if (offset < size &&
            !read_length_prefixed_text(data, size, offset, detail)) {
            return false;
        }
        if (!detail.empty()) {
            if (!out.error_text.empty()) out.error_text += ": ";
            out.error_text += detail;
        }
        return offset == size;
    }

    if (out.command != 0x84 && out.command != 0x85) return false;
    if (size != NcpHeaderBytes + 2 + DateBytes ||
        read_u16_le(data + NcpHeaderBytes) != DateBytes ||
        !valid_datetime(data + NcpHeaderBytes + 2)) {
        return false;
    }

    out.datetime.assign(
        reinterpret_cast<const char *>(data + NcpHeaderBytes + 2),
        DateBytes);
    return true;
}

}  // namespace aircannect
