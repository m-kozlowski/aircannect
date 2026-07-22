#include "report_records.h"

#include "report_records_internal.h"

#include <string.h>

namespace aircannect {
namespace {

using report_records_detail::SERIES_V2_HEADER_SIZE;
using report_records_detail::SERIES_V2_MODE_UNIFORM;
using report_records_detail::SERIES_V2_MAGIC;
using report_records_detail::SERIES_V2_VALUE_INT32_MILLI;
using report_records_detail::bitmap_bytes_for_count;
using report_records_detail::bitmap_missing;
using report_records_detail::put_le32;
using report_records_detail::put_series_v2_header;
using report_records_detail::series_v2_size;
using report_records_detail::valid_missing_bitmap;

}  // namespace

size_t report_series_v2_uniform_wire_size(uint32_t sample_count,
                                          size_t missing_bitmap_bytes) {
    if (sample_count == 0 ||
        !valid_missing_bitmap(sample_count, missing_bitmap_bytes)) {
        return 0;
    }

    size_t out = 0;
    return series_v2_size(sample_count, missing_bitmap_bytes, out)
               ? out
               : 0;
}

size_t report_series_payload_v2_uniform_slice_size(
    const ReportSeriesV2UniformView &view,
    uint32_t first_sample,
    uint32_t sample_count) {
    if (view.interval_ms == 0 || sample_count == 0 ||
        first_sample > view.sample_count ||
        sample_count > view.sample_count - first_sample) {
        return 0;
    }

    const size_t bitmap_bytes = view.missing_bitmap_bytes > 0
        ? bitmap_bytes_for_count(sample_count)
        : 0;
    return report_series_v2_uniform_wire_size(sample_count, bitmap_bytes);
}

bool report_write_series_payload_v2_uniform_slice(
    const ReportSeriesV2UniformView &view,
    uint32_t first_sample,
    uint32_t sample_count,
    uint8_t *out,
    size_t out_size) {
    const size_t expected = report_series_payload_v2_uniform_slice_size(
        view, first_sample, sample_count);
    if (!out || expected == 0 || out_size != expected) return false;

    const size_t bitmap_bytes = view.missing_bitmap_bytes > 0
        ? bitmap_bytes_for_count(sample_count)
        : 0;
    put_le32(out + 0, SERIES_V2_MAGIC);
    put_le32(out + 4, SERIES_V2_MODE_UNIFORM);
    put_le32(out + 8, view.interval_ms);
    put_le32(out + 12, sample_count);
    put_le32(out + 16, static_cast<uint32_t>(bitmap_bytes));
    put_le32(out + 20, SERIES_V2_VALUE_INT32_MILLI);

    uint8_t *bitmap = bitmap_bytes > 0
        ? out + SERIES_V2_HEADER_SIZE
        : nullptr;
    if (bitmap) {
        memset(bitmap, 0, bitmap_bytes);
        for (uint32_t i = 0; i < sample_count; ++i) {
            if (!bitmap_missing(view.missing_bitmap,
                                view.missing_bitmap_bytes,
                                first_sample + i)) {
                continue;
            }

            bitmap[i / 8u] |= static_cast<uint8_t>(1u << (i % 8u));
        }
    }

    uint8_t *values = out + SERIES_V2_HEADER_SIZE + bitmap_bytes;
    memcpy(values,
           view.values_milli_le + static_cast<size_t>(first_sample) * 4u,
           static_cast<size_t>(sample_count) * 4u);
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
}  // namespace aircannect
