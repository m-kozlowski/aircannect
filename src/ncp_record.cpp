#include "ncp_record.h"

#include "little_endian.h"

namespace aircannect {
namespace {

uint16_t read_u16(std::string_view bytes) {
    return LittleEndian::get_le16(
        reinterpret_cast<const uint8_t *>(bytes.data()));
}

bool take_field(std::string_view &bytes, std::string_view &field) {
    if (bytes.size() < 2) return false;

    const size_t length = read_u16(bytes);
    bytes.remove_prefix(2);
    if (length > bytes.size()) return false;

    field = bytes.substr(0, length);
    bytes.remove_prefix(length);
    return true;
}

}  // namespace

bool decode_ncp_record(std::string_view bytes, NcpRecordView &out) {
    out = {};
    if (bytes.size() < 4 || read_u16(bytes) != bytes.size() - 2) return false;

    out.command = static_cast<uint8_t>(bytes[2]);
    out.tag = static_cast<uint8_t>(bytes[3]);
    out.payload = bytes.substr(4);
    return true;
}

bool encode_ncp_record(uint8_t command, uint8_t tag,
                       std::string_view payload, std::string &out) {
    if (tag == 0xff || payload.size() > UINT16_MAX - 2) return false;

    out.resize(4 + payload.size());
    LittleEndian::put_le16(reinterpret_cast<uint8_t *>(&out[0]),
                           static_cast<uint16_t>(2 + payload.size()));
    out[2] = static_cast<char>(command);
    out[3] = static_cast<char>(tag);
    if (!payload.empty()) {
        out.replace(4, payload.size(), payload.data(), payload.size());
    }
    return true;
}

bool decode_ncp_error(std::string_view payload, NcpErrorView &out) {
    out = {};
    if (payload.size() < 2) return false;

    out.code = static_cast<int16_t>(read_u16(payload));
    payload.remove_prefix(2);
    if (!take_field(payload, out.message)) return false;

    // Older clock replies omit the optional detail field.
    if (!payload.empty() && !take_field(payload, out.detail)) return false;
    return payload.empty();
}

}  // namespace aircannect
