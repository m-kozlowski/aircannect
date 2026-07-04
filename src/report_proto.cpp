#include "report_proto.h"

namespace aircannect {

bool report_proto_read_varint(const uint8_t *data,
                              size_t len,
                              size_t &index,
                              uint64_t &out) {
    uint64_t value = 0;
    uint8_t shift = 0;
    while (index < len && shift < 64) {
        const uint8_t byte = data[index++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool report_proto_next(const uint8_t *data,
                       size_t len,
                       size_t &index,
                       ReportProtoField &out) {
    uint64_t key = 0;
    if (!report_proto_read_varint(data, len, index, key)) return false;
    out = {};
    out.field = static_cast<uint32_t>(key >> 3);
    out.wire = static_cast<uint8_t>(key & 0x07);
    switch (out.wire) {
        case 0:
            return report_proto_read_varint(data, len, index, out.value);
        case 1:
            if (len - index < 8) return false;
            out.data = data + index;
            out.len = 8;
            index += 8;
            return true;
        case 2: {
            uint64_t field_len = 0;
            if (!report_proto_read_varint(data,
                                          len,
                                          index,
                                          field_len)) {
                return false;
            }
            if (field_len > len - index) return false;
            out.data = data + index;
            out.len = static_cast<size_t>(field_len);
            index += out.len;
            return true;
        }
        case 5:
            if (len - index < 4) return false;
            out.data = data + index;
            out.len = 4;
            index += 4;
            return true;
        default:
            return false;
    }
}

bool report_proto_all_length_fields(const uint8_t *data,
                                    size_t len,
                                    uint32_t field_id) {
    size_t index = 0;
    bool any = false;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.field != field_id || field.wire != 2) return false;
        any = true;
    }
    return any;
}

}  // namespace aircannect
