#include "report_summary_record_codec.h"

#include <algorithm>
#include <string.h>

namespace aircannect {
namespace {

constexpr size_t SUMMARY_SESSION_OFFSET = 64;
constexpr size_t SUMMARY_SESSION_SIZE = 12;
constexpr size_t SUMMARY_FIELD_OFFSET = 256;
constexpr size_t SUMMARY_FIELD_MASK_OFFSET = SUMMARY_FIELD_OFFSET;
constexpr size_t SUMMARY_FIELD_VALUE_OFFSET = SUMMARY_FIELD_OFFSET + 8;
static_assert(SUMMARY_FIELD_VALUE_OFFSET +
                      AC_REPORT_SUMMARY_FIELD_COUNT * sizeof(uint32_t) <=
                  AC_REPORT_SUMMARY_RECORD_CODEC_SIZE,
              "summary field block must fit in summary record");

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

int32_t float_to_milli(float value) {
    if (value >= 0.0f) {
        return static_cast<int32_t>(value * 1000.0f + 0.5f);
    }
    return static_cast<int32_t>(value * 1000.0f - 0.5f);
}

float milli_to_float(int32_t value) {
    return static_cast<float>(value) / 1000.0f;
}

}  // namespace

bool report_summary_record_encode(uint8_t *raw,
                                  size_t raw_size,
                                  const ReportSummaryRecord &record) {
    if (!raw || raw_size < AC_REPORT_SUMMARY_RECORD_CODEC_SIZE) return false;
    memset(raw, 0, AC_REPORT_SUMMARY_RECORD_CODEC_SIZE);

    uint32_t flags = 0;
    if (record.valid) flags |= (1u << 0);
    if (record.has_tz_offset_min) flags |= (1u << 1);
    if (record.has_ahi) flags |= (1u << 2);
    if (record.has_apnea_index) flags |= (1u << 3);
    if (record.has_hypopnea_index) flags |= (1u << 4);
    if (record.has_oa_index) flags |= (1u << 5);
    if (record.has_ca_index) flags |= (1u << 6);
    if (record.has_ua_index) flags |= (1u << 7);
    if (record.has_rera_index) flags |= (1u << 8);
    if (record.has_session_count) flags |= (1u << 9);
    if (record.session_interval_count > 0) flags |= (1u << 10);
    if (record.summary_field_mask != 0) flags |= (1u << 11);

    put_le32(raw + 0, flags);
    put_le64(raw + 4, record.start_ms);
    put_le64(raw + 12, record.end_ms);
    put_le32(raw + 20, record.duration_min);
    put_le32(raw + 24, static_cast<uint32_t>(record.tz_offset_min));
    put_le32(raw + 28, record.session_count);
    put_le32(raw + 32, static_cast<uint32_t>(float_to_milli(record.ahi)));
    put_le32(raw + 36,
             static_cast<uint32_t>(float_to_milli(record.apnea_index)));
    put_le32(raw + 40,
             static_cast<uint32_t>(float_to_milli(record.hypopnea_index)));
    put_le32(raw + 44,
             static_cast<uint32_t>(float_to_milli(record.oa_index)));
    put_le32(raw + 48,
             static_cast<uint32_t>(float_to_milli(record.ca_index)));
    put_le32(raw + 52,
             static_cast<uint32_t>(float_to_milli(record.ua_index)));
    put_le32(raw + 56,
             static_cast<uint32_t>(float_to_milli(record.rera_index)));
    const uint32_t session_count = static_cast<uint32_t>(
        std::min<size_t>(record.session_interval_count,
                         AC_REPORT_SUMMARY_SESSION_MAX));
    put_le32(raw + 60, session_count);
    for (uint32_t i = 0; i < session_count; ++i) {
        uint8_t *dst = raw + SUMMARY_SESSION_OFFSET +
                       static_cast<size_t>(i) * SUMMARY_SESSION_SIZE;
        put_le64(dst, record.sessions[i].start_ms);
        put_le32(dst + 8, record.sessions[i].duration_min);
    }
    put_le64(raw + SUMMARY_FIELD_MASK_OFFSET, record.summary_field_mask);
    for (size_t i = 0; i < AC_REPORT_SUMMARY_FIELD_COUNT; ++i) {
        put_le32(raw + SUMMARY_FIELD_VALUE_OFFSET + i * sizeof(uint32_t),
                 record.summary_field_values[i]);
    }
    return true;
}

bool report_summary_record_decode(const uint8_t *raw,
                                  size_t raw_size,
                                  ReportSummaryRecord &record) {
    record = {};
    if (!raw || raw_size < AC_REPORT_SUMMARY_RECORD_CODEC_SIZE) return false;

    const uint32_t flags = get_le32(raw + 0);
    record.valid = (flags & (1u << 0)) != 0;
    record.has_tz_offset_min = (flags & (1u << 1)) != 0;
    record.has_ahi = (flags & (1u << 2)) != 0;
    record.has_apnea_index = (flags & (1u << 3)) != 0;
    record.has_hypopnea_index = (flags & (1u << 4)) != 0;
    record.has_oa_index = (flags & (1u << 5)) != 0;
    record.has_ca_index = (flags & (1u << 6)) != 0;
    record.has_ua_index = (flags & (1u << 7)) != 0;
    record.has_rera_index = (flags & (1u << 8)) != 0;
    record.has_session_count = (flags & (1u << 9)) != 0;
    record.start_ms = get_le64(raw + 4);
    record.end_ms = get_le64(raw + 12);
    record.duration_min = get_le32(raw + 20);
    record.tz_offset_min = static_cast<int32_t>(get_le32(raw + 24));
    record.session_count = get_le32(raw + 28);
    record.ahi = milli_to_float(static_cast<int32_t>(get_le32(raw + 32)));
    record.apnea_index =
        milli_to_float(static_cast<int32_t>(get_le32(raw + 36)));
    record.hypopnea_index =
        milli_to_float(static_cast<int32_t>(get_le32(raw + 40)));
    record.oa_index =
        milli_to_float(static_cast<int32_t>(get_le32(raw + 44)));
    record.ca_index =
        milli_to_float(static_cast<int32_t>(get_le32(raw + 48)));
    record.ua_index =
        milli_to_float(static_cast<int32_t>(get_le32(raw + 52)));
    record.rera_index =
        milli_to_float(static_cast<int32_t>(get_le32(raw + 56)));
    const uint32_t raw_session_count = static_cast<uint32_t>(
        std::min<size_t>(get_le32(raw + 60),
                         AC_REPORT_SUMMARY_SESSION_MAX));
    for (uint32_t i = 0; i < raw_session_count; ++i) {
        const uint8_t *src = raw + SUMMARY_SESSION_OFFSET +
                             static_cast<size_t>(i) * SUMMARY_SESSION_SIZE;
        const uint64_t start_ms = get_le64(src);
        const uint32_t duration_min = get_le32(src + 8);
        if (!start_ms || !duration_min) continue;
        ReportSummarySession &session =
            record.sessions[record.session_interval_count++];
        session.start_ms = start_ms;
        session.duration_min = duration_min;
    }
    if (!record.has_session_count && record.session_interval_count > 0) {
        record.has_session_count = true;
        record.session_count = record.session_interval_count;
    }
    record.summary_field_mask = get_le64(raw + SUMMARY_FIELD_MASK_OFFSET);
    for (size_t i = 0; i < AC_REPORT_SUMMARY_FIELD_COUNT; ++i) {
        record.summary_field_values[i] =
            get_le32(raw + SUMMARY_FIELD_VALUE_OFFSET + i * sizeof(uint32_t));
    }
    return record.valid && record.start_ms > 0 &&
           record.end_ms > record.start_ms;
}

}  // namespace aircannect
