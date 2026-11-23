#pragma once

#include "little_endian.h"

namespace aircannect {

struct RingBinLayout {
    enum Format { Original, O2RingS } format = Original;
    static constexpr size_t MaxStride = 5, SHeader = 10, SFooter = 48;

    size_t stride() const { return format == Original ? MaxStride : 3; }
    uint64_t offset(uint32_t sample) const {
        return (format == Original ? 40 : SHeader) + uint64_t(sample) * stride();
    }
    size_t footer() const { return format == Original ? 0 : SFooter; }

    static bool s_footer_matches(const uint8_t *p, size_t n, uint64_t size) {
        return n == SFooter && LittleEndian::get_le32(p + 4) == 0xda5a1248 &&
               p[16] == 1 && p[17] == 1 && p[18] == 3 &&
               RingBinLayout{O2RingS}.offset(LittleEndian::get_le32(p + 12)) + SFooter == size;
    }

    struct Sample { int oxygen, pulse, motion; bool invalid_vitals; };
    Sample decode(const uint8_t *p) const {
        if (format == O2RingS) return {p[0], p[1], 2 * p[2], false};
        return {p[0], p[1], p[3], p[2] != 0};
    }
};

}  // namespace aircannect
