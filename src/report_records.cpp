#include "report_records.h"

namespace aircannect {
namespace {

constexpr size_t SERIES_SAMPLE_WIRE_SIZE = 12;
constexpr size_t EVENT_RECORD_WIRE_SIZE = 16;

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

}  // namespace

size_t report_series_sample_wire_size() {
    return SERIES_SAMPLE_WIRE_SIZE;
}

size_t report_event_record_wire_size() {
    return EVENT_RECORD_WIRE_SIZE;
}

bool report_append_series_sample(ReportSpoolBuffer &out,
                                 const ReportSeriesSample &sample) {
    if (!valid_timestamp(sample.timestamp_ms)) return false;
    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(SERIES_SAMPLE_WIRE_SIZE, offset);
    if (!dst) return false;
    put_le64(dst + 0, static_cast<uint64_t>(sample.timestamp_ms));
    put_le32(dst + 8, static_cast<uint32_t>(sample.value_milli));
    return true;
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

bool report_read_series_sample(const uint8_t *data,
                               size_t len,
                               size_t index,
                               ReportSeriesSample &sample) {
    if (!data || index > SIZE_MAX / SERIES_SAMPLE_WIRE_SIZE) return false;
    const size_t offset = index * SERIES_SAMPLE_WIRE_SIZE;
    if (offset > len || len - offset < SERIES_SAMPLE_WIRE_SIZE) return false;
    sample.timestamp_ms = static_cast<int64_t>(get_le64(data + offset));
    sample.value_milli = static_cast<int32_t>(get_le32(data + offset + 8));
    return valid_timestamp(sample.timestamp_ms);
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
