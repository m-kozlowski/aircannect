#include "report_records.h"

#include "report_records_internal.h"

namespace aircannect {
namespace {

using report_records_detail::SERIES_V2_HEADER_SIZE;
using report_records_detail::SERIES_V2_MODE_EXPLICIT;
using report_records_detail::SERIES_V2_MODE_UNIFORM;
using report_records_detail::put_le32;
using report_records_detail::put_series_v2_header;
using report_records_detail::series_v2_size;
using report_records_detail::valid_timestamp;
using report_records_detail::valid_missing_bitmap;

}  // namespace

size_t report_series_v2_header_size() {
    return SERIES_V2_HEADER_SIZE;
}

size_t report_series_v2_uniform_wire_size(uint32_t sample_count,
                                          size_t missing_bitmap_bytes) {
    if (sample_count == 0 ||
        !valid_missing_bitmap(sample_count, missing_bitmap_bytes)) {
        return 0;
    }

    size_t out = 0;
    return series_v2_size(sample_count, missing_bitmap_bytes, 4, out)
               ? out
               : 0;
}

size_t report_series_v2_explicit_wire_size(uint32_t sample_count) {
    if (sample_count == 0) return 0;

    size_t out = 0;
    return series_v2_size(sample_count, 0, 8, out) ? out : 0;
}

bool report_build_series_payload_v2_uniform(
    ReportSpoolBuffer &out,
    uint32_t interval_ms,
    const int32_t *values_milli,
    uint32_t sample_count,
    const uint8_t *missing_bitmap,
    size_t missing_bitmap_bytes) {
    out.clear();

    if (!values_milli || interval_ms == 0 || sample_count == 0 ||
        (missing_bitmap_bytes && !missing_bitmap)) {
        return false;
    }

    const size_t size =
        report_series_v2_uniform_wire_size(sample_count,
                                           missing_bitmap_bytes);
    if (size == 0 || !out.reserve_capacity(size)) return false;

    if (!put_series_v2_header(out,
                              SERIES_V2_MODE_UNIFORM,
                              interval_ms,
                              sample_count,
                              missing_bitmap_bytes)) {
        out.clear();
        return false;
    }

    if (missing_bitmap_bytes &&
        !out.append(missing_bitmap, missing_bitmap_bytes)) {
        out.clear();
        return false;
    }

    for (uint32_t i = 0; i < sample_count; ++i) {
        size_t offset = 0;
        uint8_t *dst = out.append_uninitialized(4, offset);
        if (!dst) {
            out.clear();
            return false;
        }

        put_le32(dst, static_cast<uint32_t>(values_milli[i]));
    }

    return true;
}

bool report_build_series_payload_v2_uniform_values_le(
    ReportSpoolBuffer &out,
    uint32_t interval_ms,
    const uint8_t *values_milli_le,
    uint32_t sample_count,
    const uint8_t *missing_bitmap,
    size_t missing_bitmap_bytes) {
    out.clear();

    if (!values_milli_le || interval_ms == 0 || sample_count == 0 ||
        (missing_bitmap_bytes && !missing_bitmap)) {
        return false;
    }
    if (static_cast<size_t>(sample_count) > SIZE_MAX / 4u) {
        return false;
    }

    const size_t value_bytes = static_cast<size_t>(sample_count) * 4u;
    const size_t size =
        report_series_v2_uniform_wire_size(sample_count,
                                           missing_bitmap_bytes);
    if (size == 0 || !out.reserve_capacity(size)) return false;

    if (!put_series_v2_header(out,
                              SERIES_V2_MODE_UNIFORM,
                              interval_ms,
                              sample_count,
                              missing_bitmap_bytes)) {
        out.clear();
        return false;
    }

    if (missing_bitmap_bytes &&
        !out.append(missing_bitmap, missing_bitmap_bytes)) {
        out.clear();
        return false;
    }
    if (!out.append(values_milli_le, value_bytes)) {
        out.clear();
        return false;
    }

    return true;
}

bool report_build_series_payload_v2_explicit(
    ReportSpoolBuffer &out,
    int64_t chunk_start_ms,
    const ReportSeriesSample *samples,
    uint32_t sample_count) {
    out.clear();

    if (!valid_timestamp(chunk_start_ms) || !samples || sample_count == 0) {
        return false;
    }

    const size_t size = report_series_v2_explicit_wire_size(sample_count);
    if (size == 0 || !out.reserve_capacity(size)) return false;

    if (!put_series_v2_header(out,
                              SERIES_V2_MODE_EXPLICIT,
                              0,
                              sample_count,
                              0)) {
        out.clear();
        return false;
    }

    for (uint32_t i = 0; i < sample_count; ++i) {
        if (!valid_timestamp(samples[i].timestamp_ms)) {
            out.clear();
            return false;
        }

        const int64_t offset_ms = samples[i].timestamp_ms - chunk_start_ms;
        if (offset_ms < INT32_MIN || offset_ms > INT32_MAX) {
            out.clear();
            return false;
        }

        size_t offset = 0;
        uint8_t *dst = out.append_uninitialized(8, offset);
        if (!dst) {
            out.clear();
            return false;
        }

        put_le32(dst + 0,
                 static_cast<uint32_t>(static_cast<int32_t>(offset_ms)));
        put_le32(dst + 4, static_cast<uint32_t>(samples[i].value_milli));
    }

    return true;
}

}  // namespace aircannect
