#include "report_plot_payload.h"

#include <stddef.h>

namespace aircannect {

PlotBlobScan scan_plot_blob(const ReportSpoolBuffer &b) {
    PlotBlobScan out;
    const uint8_t *d = reinterpret_cast<const uint8_t *>(b.data());
    const size_t n = b.size();
    if (!d || n < 20) return out;

    if (read_u32_le(d) != PLOT_BIN_MAGIC ||
        read_u16_le(d + 4) != PLOT_BIN_VERSION) {
        return out;
    }

    const uint32_t ev = read_u32_le(d + 16);
    out.events = ev;
    size_t off = 20 + static_cast<size_t>(ev) * 16;
    if (off > n) return out;

    while (off < n) {
        if (off + 2 > n) return out;

        const uint16_t name_len = read_u16_le(d + off);
        off += 2;
        if (off + name_len + 4 > n) return out;

        off += name_len;
        const uint8_t mode = d[off++];
        off += 1;  // flags
        off += 2;  // reserved

        if (mode == PLOT_SERIES_MODE_COMPACT) {
            if (off + 16 > n) return out;
            off += 4;  // series_base_delta_ms

            const uint32_t time_unit_ms = read_u32_le(d + off);
            off += 4;
            const uint32_t value_scale_milli = read_u32_le(d + off);
            off += 4;
            const uint32_t pc = read_u32_le(d + off);
            off += 4;
            if (time_unit_ms == 0 || value_scale_milli == 0) return out;
            if (off + static_cast<size_t>(pc) * 4 > n) return out;

            off += static_cast<size_t>(pc) * 4;
            out.points += pc;
            continue;
        }

        if (mode == PLOT_SERIES_MODE_ENVELOPE_RUNS) {
            if (off + 16 > n) return out;
            off += 4;  // axis_base_delta_ms

            const uint32_t bucket_ms = read_u32_le(d + off);
            off += 4;
            const uint32_t value_scale_milli = read_u32_le(d + off);
            off += 4;
            const uint32_t run_count = read_u32_le(d + off);
            off += 4;
            if (bucket_ms == 0 || value_scale_milli == 0) return out;

            for (uint32_t i = 0; i < run_count; ++i) {
                if (off + 6 > n) return out;
                off += 4;  // start_bucket

                const uint16_t bucket_count = read_u16_le(d + off);
                off += 2;
                if (off + static_cast<size_t>(bucket_count) * 4 > n) {
                    return out;
                }

                off += static_cast<size_t>(bucket_count) * 4;
                out.points += static_cast<uint32_t>(bucket_count) * 2u;
            }
            continue;
        }

        return out;
    }

    out.valid = off == n;
    return out;
}

}  // namespace aircannect
