#include "report_records.h"

#include <limits.h>

namespace aircannect {
namespace {

constexpr size_t SERIES_V2_HEADER_SIZE = 24;
constexpr size_t EVENT_RECORD_WIRE_SIZE = 16;
constexpr uint32_t SERIES_V2_MAGIC = 0x32535241u;  // "ARS2"
constexpr uint32_t SERIES_V2_MODE_UNIFORM = 1;
constexpr uint32_t SERIES_V2_MODE_EXPLICIT = 2;
constexpr uint32_t SERIES_V2_VALUE_INT32_MILLI = 1;

void put_le16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void put_le32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void put_le64(uint8_t *out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }
}

uint16_t get_le16(const uint8_t *in) {
    return static_cast<uint16_t>(in[0]) |
           (static_cast<uint16_t>(in[1]) << 8);
}

uint32_t get_le32(const uint8_t *in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t get_le64(const uint8_t *in) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[i]) << (i * 8);
    }
    return value;
}

bool valid_timestamp(int64_t timestamp_ms) {
    return timestamp_ms > 0;
}

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

size_t bitmap_bytes_for_count(uint32_t sample_count) {
    return (static_cast<size_t>(sample_count) + 7u) / 8u;
}

bool bitmap_missing(const uint8_t *bitmap, size_t bitmap_bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bitmap || byte >= bitmap_bytes) return false;
    return (bitmap[byte] & static_cast<uint8_t>(1u << (index % 8u))) != 0;
}

bool valid_missing_bitmap(uint32_t sample_count, size_t bitmap_bytes) {
    return bitmap_bytes <= bitmap_bytes_for_count(sample_count);
}

bool series_v2_size(uint32_t sample_count,
                    size_t missing_bitmap_bytes,
                    size_t value_bytes_per_sample,
                    size_t &out) {
    size_t values = 0;
    if (!checked_mul_size(static_cast<size_t>(sample_count),
                          value_bytes_per_sample,
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
                            uint32_t &mode,
                            uint32_t &interval_ms,
                            uint32_t &sample_count,
                            size_t &missing_bitmap_bytes,
                            const uint8_t *&missing_bitmap,
                            const uint8_t *&body,
                            size_t &body_len) {
    mode = 0;
    interval_ms = 0;
    sample_count = 0;
    missing_bitmap_bytes = 0;
    missing_bitmap = nullptr;
    body = nullptr;
    body_len = 0;
    if (!data || len < SERIES_V2_HEADER_SIZE || record_count == 0) {
        return false;
    }
    if (get_le32(data + 0) != SERIES_V2_MAGIC) return false;
    mode = get_le32(data + 4);
    interval_ms = get_le32(data + 8);
    sample_count = get_le32(data + 12);
    missing_bitmap_bytes = get_le32(data + 16);
    const uint32_t value_encoding = get_le32(data + 20);
    if (value_encoding != SERIES_V2_VALUE_INT32_MILLI ||
        sample_count == 0 || sample_count != record_count ||
        !valid_missing_bitmap(sample_count, missing_bitmap_bytes)) {
        return false;
    }
    if (len < SERIES_V2_HEADER_SIZE + missing_bitmap_bytes) return false;
    missing_bitmap = missing_bitmap_bytes
                         ? data + SERIES_V2_HEADER_SIZE
                         : nullptr;
    body = data + SERIES_V2_HEADER_SIZE + missing_bitmap_bytes;
    body_len = len - SERIES_V2_HEADER_SIZE - missing_bitmap_bytes;
    return true;
}

bool put_series_v2_header(ReportSpoolBuffer &out,
                          uint32_t mode,
                          uint32_t interval_ms,
                          uint32_t sample_count,
                          size_t missing_bitmap_bytes) {
    if (sample_count == 0 ||
        missing_bitmap_bytes > UINT32_MAX ||
        !valid_missing_bitmap(sample_count, missing_bitmap_bytes)) {
        return false;
    }
    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(SERIES_V2_HEADER_SIZE, offset);
    if (!dst) return false;
    put_le32(dst + 0, SERIES_V2_MAGIC);
    put_le32(dst + 4, mode);
    put_le32(dst + 8, interval_ms);
    put_le32(dst + 12, sample_count);
    put_le32(dst + 16, static_cast<uint32_t>(missing_bitmap_bytes));
    put_le32(dst + 20, SERIES_V2_VALUE_INT32_MILLI);
    return true;
}

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

size_t report_event_record_wire_size() {
    return EVENT_RECORD_WIRE_SIZE;
}

bool report_append_event_record(ReportSpoolBuffer &out,
                                const ReportEventRecord &event) {
    if (!valid_timestamp(event.start_ms) || event.duration_ms < 0) {
        return false;
    }
    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(EVENT_RECORD_WIRE_SIZE, offset);
    if (!dst) return false;
    put_le64(dst + 0, static_cast<uint64_t>(event.start_ms));
    put_le32(dst + 8, static_cast<uint32_t>(event.duration_ms));
    put_le16(dst + 12, event.code);
    put_le16(dst + 14, event.flags);
    return true;
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
        put_le32(dst + 0, static_cast<uint32_t>(
                              static_cast<int32_t>(offset_ms)));
        put_le32(dst + 4, static_cast<uint32_t>(samples[i].value_milli));
    }
    return true;
}

bool report_series_payload_v2_uniform_view(
    const uint8_t *data,
    size_t len,
    uint32_t record_count,
    ReportSeriesV2UniformView &view) {
    view = {};

    uint32_t mode = 0;
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    size_t missing_bitmap_bytes = 0;
    const uint8_t *missing_bitmap = nullptr;
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    if (!parse_series_v2_header(data,
                                len,
                                record_count,
                                mode,
                                interval_ms,
                                sample_count,
                                missing_bitmap_bytes,
                                missing_bitmap,
                                body,
                                body_len)) {
        return false;
    }
    if (mode != SERIES_V2_MODE_UNIFORM ||
        interval_ms == 0 ||
        static_cast<size_t>(sample_count) > SIZE_MAX / 4u ||
        body_len != static_cast<size_t>(sample_count) * 4u) {
        return false;
    }

    view.interval_ms = interval_ms;
    view.sample_count = sample_count;
    view.missing_bitmap = missing_bitmap;
    view.missing_bitmap_bytes = missing_bitmap_bytes;
    view.values_milli_le = body;
    view.values_milli_bytes = body_len;
    return true;
}

bool report_for_each_series_sample(uint32_t payload_schema,
                                   int64_t chunk_start_ms,
                                   const uint8_t *data,
                                   size_t len,
                                   uint32_t record_count,
                                   ReportSeriesSampleCallback callback,
                                   void *context) {
    if (!callback || record_count == 0 ||
        payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2 ||
        !valid_timestamp(chunk_start_ms)) {
        return false;
    }

    uint32_t mode = 0;
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    size_t missing_bitmap_bytes = 0;
    const uint8_t *missing_bitmap = nullptr;
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    if (!parse_series_v2_header(data,
                                len,
                                record_count,
                                mode,
                                interval_ms,
                                sample_count,
                                missing_bitmap_bytes,
                                missing_bitmap,
                                body,
                                body_len)) {
        return false;
    }

    if (mode == SERIES_V2_MODE_UNIFORM) {
        if (interval_ms == 0 ||
            static_cast<size_t>(sample_count) > SIZE_MAX / 4u ||
            body_len != static_cast<size_t>(sample_count) * 4u) {
            return false;
        }
        if (static_cast<int64_t>(interval_ms) >
            INT64_MAX - chunk_start_ms) {
            return false;
        }
        const int64_t max_steps =
            (INT64_MAX - chunk_start_ms) /
            static_cast<int64_t>(interval_ms);
        if (static_cast<int64_t>(sample_count - 1) > max_steps) {
            return false;
        }
        for (uint32_t i = 0; i < sample_count; ++i) {
            if (bitmap_missing(missing_bitmap, missing_bitmap_bytes, i)) {
                continue;
            }
            ReportSeriesSample sample;
            sample.timestamp_ms =
                chunk_start_ms +
                static_cast<int64_t>(i) *
                    static_cast<int64_t>(interval_ms);
            sample.value_milli =
                static_cast<int32_t>(get_le32(body + i * 4u));
            if (!callback(context, sample)) return false;
        }
        return true;
    }

    if (mode == SERIES_V2_MODE_EXPLICIT) {
        if (interval_ms != 0 || missing_bitmap_bytes != 0 ||
            static_cast<size_t>(sample_count) > SIZE_MAX / 8u ||
            body_len != static_cast<size_t>(sample_count) * 8u) {
            return false;
        }
        for (uint32_t i = 0; i < sample_count; ++i) {
            const uint8_t *record = body + i * 8u;
            const int32_t offset_ms =
                static_cast<int32_t>(get_le32(record + 0));
            const int64_t timestamp_ms =
                chunk_start_ms + static_cast<int64_t>(offset_ms);
            if (!valid_timestamp(timestamp_ms)) return false;
            ReportSeriesSample sample;
            sample.timestamp_ms = timestamp_ms;
            sample.value_milli =
                static_cast<int32_t>(get_le32(record + 4));
            if (!callback(context, sample)) return false;
        }
        return true;
    }
    return false;
}

bool report_read_event_record(const uint8_t *data,
                              size_t len,
                              size_t index,
                              ReportEventRecord &event) {
    if (!data || index > SIZE_MAX / EVENT_RECORD_WIRE_SIZE) return false;
    const size_t offset = index * EVENT_RECORD_WIRE_SIZE;
    if (offset > len || len - offset < EVENT_RECORD_WIRE_SIZE) return false;
    event.start_ms = static_cast<int64_t>(get_le64(data + offset));
    event.duration_ms = static_cast<int32_t>(get_le32(data + offset + 8));
    event.code = get_le16(data + offset + 12);
    event.flags = get_le16(data + offset + 14);
    return valid_timestamp(event.start_ms) && event.duration_ms >= 0;
}

}  // namespace aircannect
