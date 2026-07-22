#include "report_plot_encoder.h"

namespace aircannect {

bool bin_put_u8(ReportSpoolBuffer &out, uint8_t value) {
    return out.append(&value, sizeof(value));
}

bool bin_put_u16(ReportSpoolBuffer &out, uint16_t value) {
    const uint8_t bytes[2] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
    };
    return out.append(bytes, sizeof(bytes));
}

bool bin_put_u32(ReportSpoolBuffer &out, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    return out.append(bytes, sizeof(bytes));
}

bool bin_put_i16(ReportSpoolBuffer &out, int16_t value) {
    return bin_put_u16(out, static_cast<uint16_t>(value));
}

bool bin_put_i32(ReportSpoolBuffer &out, int32_t value) {
    return bin_put_u32(out, static_cast<uint32_t>(value));
}

bool bin_put_i64(ReportSpoolBuffer &out, int64_t value) {
    const uint64_t encoded = static_cast<uint64_t>(value);
    return bin_put_u32(out, static_cast<uint32_t>(encoded)) &&
           bin_put_u32(out, static_cast<uint32_t>(encoded >> 32));
}

uint16_t read_u16_le(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

uint32_t read_u32_le(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

int32_t read_i32_le(const uint8_t *data) {
    return static_cast<int32_t>(read_u32_le(data));
}

}  // namespace aircannect
