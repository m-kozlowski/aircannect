#include "report_records_internal.h"

#include <limits.h>

namespace aircannect {
namespace report_records_detail {
namespace {

bool checked_add_size(size_t a, size_t b, size_t &out) {
    if (b > SIZE_MAX - a) return false;

    out = a + b;
    return true;
}

bool checked_mul_size(size_t a, size_t b, size_t &out) {
    if (a != 0 && b > SIZE_MAX / a) return false;

    out = a * b;
    return true;
}

}  // namespace

size_t bitmap_bytes_for_count(uint32_t sample_count) {
    return (static_cast<size_t>(sample_count) + 7u) / 8u;
}

bool bitmap_missing(const uint8_t *bitmap,
                    size_t bitmap_bytes,
                    uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bitmap || byte >= bitmap_bytes) return false;

    return (bitmap[byte] & static_cast<uint8_t>(1u << (index % 8u))) != 0;
}

bool valid_missing_bitmap(uint32_t sample_count, size_t bitmap_bytes) {
    return bitmap_bytes <= bitmap_bytes_for_count(sample_count);
}

bool series_v2_size(uint32_t sample_count,
                    size_t missing_bitmap_bytes,
                    size_t &out) {
    size_t values = 0;
    if (!checked_mul_size(static_cast<size_t>(sample_count),
                          sizeof(int32_t),
                          values)) {
        return false;
    }

    size_t with_bitmap = 0;
    if (!checked_add_size(SERIES_V2_HEADER_SIZE,
                          missing_bitmap_bytes,
                          with_bitmap)) {
        return false;
    }

    return checked_add_size(with_bitmap, values, out);
}

bool parse_series_v2_header(const uint8_t *data,
                            size_t len,
                            uint32_t record_count,
                            SeriesV2HeaderView &view) {
    view = {};

    if (!data || len < SERIES_V2_HEADER_SIZE || record_count == 0) {
        return false;
    }
    if (get_le32(data + 0) != SERIES_V2_MAGIC) return false;

    view.mode = get_le32(data + 4);
    view.interval_ms = get_le32(data + 8);
    view.sample_count = get_le32(data + 12);
    view.missing_bitmap_bytes = get_le32(data + 16);
    const uint32_t value_encoding = get_le32(data + 20);

    if (view.mode != SERIES_V2_MODE_UNIFORM ||
        value_encoding != SERIES_V2_VALUE_INT32_MILLI ||
        view.interval_ms == 0 ||
        view.sample_count == 0 || view.sample_count != record_count ||
        !valid_missing_bitmap(view.sample_count,
                              view.missing_bitmap_bytes)) {
        return false;
    }
    if (len < SERIES_V2_HEADER_SIZE + view.missing_bitmap_bytes) {
        return false;
    }

    view.missing_bitmap = view.missing_bitmap_bytes
                              ? data + SERIES_V2_HEADER_SIZE
                              : nullptr;
    view.body = data + SERIES_V2_HEADER_SIZE + view.missing_bitmap_bytes;
    view.body_len =
        len - SERIES_V2_HEADER_SIZE - view.missing_bitmap_bytes;
    return true;
}

bool put_series_v2_header(ReportSpoolBuffer &out,
                          uint32_t interval_ms,
                          uint32_t sample_count,
                          size_t missing_bitmap_bytes) {
    if (interval_ms == 0 || sample_count == 0 ||
        missing_bitmap_bytes > UINT32_MAX ||
        !valid_missing_bitmap(sample_count, missing_bitmap_bytes)) {
        return false;
    }

    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(SERIES_V2_HEADER_SIZE, offset);
    if (!dst) return false;

    put_le32(dst + 0, SERIES_V2_MAGIC);
    put_le32(dst + 4, SERIES_V2_MODE_UNIFORM);
    put_le32(dst + 8, interval_ms);
    put_le32(dst + 12, sample_count);
    put_le32(dst + 16, static_cast<uint32_t>(missing_bitmap_bytes));
    put_le32(dst + 20, SERIES_V2_VALUE_INT32_MILLI);
    return true;
}

}  // namespace report_records_detail
}  // namespace aircannect
