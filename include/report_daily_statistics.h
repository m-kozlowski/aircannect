#pragma once

#include <stddef.h>
#include <stdint.h>

#include "little_endian.h"

namespace aircannect {

// STR supplies selected percentiles, not the underlying distribution.
struct ReportDailyStatistics {
    enum Signal : uint8_t {
        MaskPressure, Ipap, Epap, Leak, MinuteVentilation, RespiratoryRate,
        TidalVolume, Spo2, TargetVentilation, InspirationTime, IeRatio, Count,
    };
    enum Field : uint8_t { Median, P95, Max, FieldCount };
    struct Row {
        uint32_t valid_mask = 0;
        int32_t values[FieldCount] = {};
    };

    static constexpr size_t RowBytes = 16;
    static constexpr size_t WireBytes = Count * RowBytes;
    Row rows[Count];

    void encode(uint8_t *out) const {
        for (size_t i = 0; i < Count; ++i) {
            LittleEndian::put_le32(out + i * RowBytes, rows[i].valid_mask);
            for (size_t j = 0; j < FieldCount; ++j) {
                LittleEndian::put_le32(out + i * RowBytes + 4 + j * 4,
                                      rows[i].values[j]);
            }
        }
    }

    bool decode(const uint8_t *in) {
        for (size_t i = 0; i < Count; ++i) {
            rows[i].valid_mask = LittleEndian::get_le32(in + i * RowBytes);
            if (rows[i].valid_mask >> FieldCount) return false;

            for (size_t j = 0; j < FieldCount; ++j) {
                rows[i].values[j] = static_cast<int32_t>(
                    LittleEndian::get_le32(in + i * RowBytes + 4 + j * 4));
            }
        }
        return true;
    }
};

}  // namespace aircannect
