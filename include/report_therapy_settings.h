#pragma once

#include <stddef.h>
#include <stdint.h>

#include "little_endian.h"

namespace aircannect {

// STR's daily snapshot, not a claim about when settings changed within the day.
// Values are physical thousandths; absent fields have no validity bit.
struct ReportTherapySettings {
    enum Field : uint8_t {
        Mode, Pressure, MinPressure, MaxPressure, Ipap, Epap, MaxIpap,
        MinEpap, MaxEpap, Ps, MinPs, MaxPs, TiMin, TiMax, Ti, Rate,
        StartPressure, EprEnabled, EprLevel, EprType, RampEnabled,
        RampMinutes, Humidity, Count,
    };

    static constexpr size_t WireBytes = 4 + Count * 4;
    uint32_t valid_mask = 0;
    int32_t values[Count] = {};

    void encode(uint8_t *out) const {
        LittleEndian::put_le32(out, valid_mask);
        for (size_t i = 0; i < Count; ++i) {
            LittleEndian::put_le32(out + 4 + i * 4, values[i]);
        }
    }

    bool decode(const uint8_t *in) {
        valid_mask = LittleEndian::get_le32(in);
        if (valid_mask >> Count) return false;

        for (size_t i = 0; i < Count; ++i) {
            values[i] = static_cast<int32_t>(LittleEndian::get_le32(in + 4 + i * 4));
        }
        return true;
    }
};

}  // namespace aircannect
