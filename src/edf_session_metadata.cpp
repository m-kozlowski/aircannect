#include "edf_session_metadata.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "edf_storage_catalog.h"
#include "little_endian.h"

namespace aircannect {
namespace {

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

constexpr uint8_t FILE_MAGIC[8] = {
    'A', 'C', 'E', 'D', 'F', 'S', 'M', '1',
};
constexpr uint32_t FLAG_EXTERNALLY_CORRECTED = 1u << 0;
constexpr uint32_t FLAG_FINALIZED = 1u << 1;
constexpr uint32_t KNOWN_FLAGS =
    FLAG_EXTERNALLY_CORRECTED | FLAG_FINALIZED;
constexpr size_t CRC_OFFSET = EdfSessionMetadataCodec::RecordBytes - 4;
constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

bool digits(const char *text, size_t length) {
    if (!text) return false;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}

bool sleep_day_text(const char *text, SleepDayId expected) {
    SleepDayId parsed;
    return text && text[8] == '\0' && digits(text, 8) &&
           SleepDayId::from_yyyymmdd(text, parsed) && parsed == expected;
}

bool session_stamp_text(const char *text) {
    return text && text[8] == '_' && text[15] == '\0' &&
           digits(text, 8) && digits(text + 9, 6);
}

bool corrected_time(int64_t raw, int64_t correction, int64_t canonical) {
    if (correction > 0 && raw < INT64_MIN + correction) return false;
    if (correction < 0 && raw > INT64_MAX + correction) return false;
    return canonical == raw - correction;
}

bool apply_correction(int64_t raw,
                      int64_t correction,
                      int64_t &canonical) {
    if (correction > 0 && raw < INT64_MIN + correction) return false;
    if (correction < 0 && raw > INT64_MAX + correction) return false;
    canonical = raw - correction;
    return true;
}

bool sleep_day(const EdfLocalDateTime &local,
               SleepDayId &sleep_day_id,
               char *text) {
    char value[9] = {};
    if (!edf_sleep_day_yyyymmdd(local, value, sizeof(value)) ||
        !SleepDayId::from_yyyymmdd(value, sleep_day_id)) {
        return false;
    }
    if (text) memcpy(text, value, sizeof(value));
    return true;
}

bool time_pair_valid(int64_t raw,
                     int64_t canonical,
                     const EdfSessionMetadata &metadata) {
    if (raw <= 0 || canonical <= 0) return false;
    if (!metadata.externally_corrected) return raw == canonical;
    return corrected_time(raw, metadata.device_minus_utc_ms, canonical);
}

void put_i32(uint8_t *out, int32_t value) {
    put_le32(out, static_cast<uint32_t>(value));
}

void put_i64(uint8_t *out, int64_t value) {
    put_le64(out, static_cast<uint64_t>(value));
}

int32_t get_i32(const uint8_t *in) {
    return static_cast<int32_t>(get_le32(in));
}

int64_t get_i64(const uint8_t *in) {
    return static_cast<int64_t>(get_le64(in));
}

bool decode_v1(const uint8_t *bytes,
               size_t length,
               EdfSessionMetadata &metadata) {
    if (length != EdfSessionMetadataCodec::RecordBytes ||
        get_le16(bytes + 10) != EdfSessionMetadataCodec::RecordBytes ||
        get_le32(bytes + 12) != EdfSessionMetadataCodec::RecordBytes ||
        get_le32(bytes + CRC_OFFSET) != crc32_ieee(bytes, CRC_OFFSET)) {
        return false;
    }

    const uint32_t flags = get_le32(bytes + 16);
    if ((flags & ~KNOWN_FLAGS) != 0) return false;

    EdfSessionMetadata decoded;
    decoded.externally_corrected =
        (flags & FLAG_EXTERNALLY_CORRECTED) != 0;
    decoded.finalized = (flags & FLAG_FINALIZED) != 0;
    decoded.timezone_offset_minutes = get_i32(bytes + 20);
    decoded.device_minus_utc_ms = get_i64(bytes + 24);
    decoded.raw_segment_start_ms = get_i64(bytes + 32);
    decoded.raw_segment_end_ms = get_i64(bytes + 40);
    decoded.canonical_segment_start_ms = get_i64(bytes + 48);
    decoded.canonical_segment_end_ms = get_i64(bytes + 56);
    decoded.raw_therapy_start_ms = get_i64(bytes + 64);
    decoded.raw_therapy_end_ms = get_i64(bytes + 72);
    decoded.canonical_therapy_start_ms = get_i64(bytes + 80);
    decoded.canonical_therapy_end_ms = get_i64(bytes + 88);
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 96),
                                     decoded.raw_sleep_day) ||
        !SleepDayId::from_epoch_days(get_i32(bytes + 100),
                                     decoded.canonical_sleep_day)) {
        return false;
    }

    memcpy(decoded.datalog_sleep_day, bytes + 104, 8);
    decoded.datalog_sleep_day[8] = '\0';
    memcpy(decoded.session_stamp, bytes + 112, 15);
    decoded.session_stamp[15] = '\0';
    decoded.capture_session_id = get_le32(bytes + 128);
    if (!edf_session_metadata_valid(decoded)) return false;

    metadata = decoded;
    return true;
}

}  // namespace

bool edf_session_metadata_valid(const EdfSessionMetadata &metadata) {
    if (!metadata.raw_sleep_day.valid() ||
        !metadata.canonical_sleep_day.valid() ||
        !sleep_day_text(metadata.datalog_sleep_day,
                        metadata.canonical_sleep_day) ||
        !session_stamp_text(metadata.session_stamp) ||
        metadata.timezone_offset_minutes < -24 * 60 ||
        metadata.timezone_offset_minutes > 24 * 60 ||
        metadata.capture_session_id == 0 ||
        !time_pair_valid(metadata.raw_segment_start_ms,
                         metadata.canonical_segment_start_ms,
                         metadata) ||
        !time_pair_valid(metadata.raw_therapy_start_ms,
                         metadata.canonical_therapy_start_ms,
                         metadata)) {
        return false;
    }
    if (!metadata.externally_corrected &&
        metadata.device_minus_utc_ms != 0) {
        return false;
    }
    if (!metadata.finalized) {
        return metadata.raw_segment_end_ms == 0 &&
               metadata.canonical_segment_end_ms == 0 &&
               metadata.raw_therapy_end_ms == 0 &&
               metadata.canonical_therapy_end_ms == 0;
    }

    return time_pair_valid(metadata.raw_segment_end_ms,
                           metadata.canonical_segment_end_ms,
                           metadata) &&
           time_pair_valid(metadata.raw_therapy_end_ms,
                           metadata.canonical_therapy_end_ms,
                           metadata) &&
           metadata.raw_segment_end_ms > metadata.raw_segment_start_ms &&
           metadata.canonical_segment_end_ms >
               metadata.canonical_segment_start_ms &&
           metadata.raw_therapy_end_ms > metadata.raw_therapy_start_ms &&
           metadata.canonical_therapy_end_ms >
               metadata.canonical_therapy_start_ms;
}

bool edf_session_metadata_begin(int64_t raw_segment_start_ms,
                                int64_t raw_therapy_start_ms,
                                const As11ClockTransform &clock,
                                int32_t timezone_offset_minutes,
                                uint32_t capture_session_id,
                                EdfSessionMetadata &metadata) {
    int64_t canonical_segment_start_ms = 0;
    int64_t canonical_therapy_start_ms = 0;
    if (raw_segment_start_ms <= 0 || raw_therapy_start_ms <= 0 ||
        !clock.to_utc_ms(raw_segment_start_ms,
                         canonical_segment_start_ms) ||
        !clock.to_utc_ms(raw_therapy_start_ms,
                         canonical_therapy_start_ms)) {
        return false;
    }

    EdfLocalDateTime raw_local;
    EdfLocalDateTime canonical_local;
    if (!edf_epoch_ms_to_local_datetime(raw_segment_start_ms,
                                        timezone_offset_minutes,
                                        raw_local) ||
        !edf_epoch_ms_to_local_datetime(canonical_segment_start_ms,
                                        timezone_offset_minutes,
                                        canonical_local)) {
        return false;
    }

    EdfSessionMetadata opened;
    opened.raw_segment_start_ms = raw_segment_start_ms;
    opened.canonical_segment_start_ms = canonical_segment_start_ms;
    opened.raw_therapy_start_ms = raw_therapy_start_ms;
    opened.canonical_therapy_start_ms = canonical_therapy_start_ms;
    opened.device_minus_utc_ms = clock.externally_referenced
        ? clock.device_minus_utc_ms
        : 0;
    opened.timezone_offset_minutes = timezone_offset_minutes;
    opened.capture_session_id = capture_session_id;
    opened.externally_corrected = clock.externally_referenced;

    if (!sleep_day(raw_local, opened.raw_sleep_day, nullptr) ||
        !sleep_day(canonical_local,
                   opened.canonical_sleep_day,
                   opened.datalog_sleep_day) ||
        !edf_session_stamp(canonical_local,
                           opened.session_stamp,
                           sizeof(opened.session_stamp)) ||
        !edf_session_metadata_valid(opened)) {
        return false;
    }

    metadata = opened;
    return true;
}

bool edf_session_metadata_finalize(EdfSessionMetadata &metadata,
                                   int64_t raw_segment_end_ms,
                                   int64_t raw_therapy_end_ms) {
    EdfSessionMetadata finalized = metadata;
    if (!finalized.externally_corrected) {
        finalized.device_minus_utc_ms = 0;
    }
    if (!apply_correction(raw_segment_end_ms,
                          finalized.device_minus_utc_ms,
                          finalized.canonical_segment_end_ms) ||
        !apply_correction(raw_therapy_end_ms,
                          finalized.device_minus_utc_ms,
                          finalized.canonical_therapy_end_ms)) {
        return false;
    }

    finalized.raw_segment_end_ms = raw_segment_end_ms;
    finalized.raw_therapy_end_ms = raw_therapy_end_ms;
    finalized.finalized = true;
    if (!edf_session_metadata_valid(finalized)) return false;

    metadata = finalized;
    return true;
}

bool EdfSessionMetadataCodec::encode(const EdfSessionMetadata &metadata,
                                     uint8_t *out,
                                     size_t out_size) {
    if (!out || out_size < RecordBytes ||
        !edf_session_metadata_valid(metadata)) {
        return false;
    }

    memset(out, 0, RecordBytes);
    memcpy(out, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_le16(out + 8, Version);
    put_le16(out + 10, RecordBytes);
    put_le32(out + 12, RecordBytes);

    uint32_t flags = metadata.externally_corrected
        ? FLAG_EXTERNALLY_CORRECTED
        : 0;
    if (metadata.finalized) flags |= FLAG_FINALIZED;
    put_le32(out + 16, flags);
    put_i32(out + 20, metadata.timezone_offset_minutes);
    put_i64(out + 24, metadata.device_minus_utc_ms);
    put_i64(out + 32, metadata.raw_segment_start_ms);
    put_i64(out + 40, metadata.raw_segment_end_ms);
    put_i64(out + 48, metadata.canonical_segment_start_ms);
    put_i64(out + 56, metadata.canonical_segment_end_ms);
    put_i64(out + 64, metadata.raw_therapy_start_ms);
    put_i64(out + 72, metadata.raw_therapy_end_ms);
    put_i64(out + 80, metadata.canonical_therapy_start_ms);
    put_i64(out + 88, metadata.canonical_therapy_end_ms);
    put_i32(out + 96, metadata.raw_sleep_day.epoch_days());
    put_i32(out + 100, metadata.canonical_sleep_day.epoch_days());
    memcpy(out + 104, metadata.datalog_sleep_day, 8);
    memcpy(out + 112, metadata.session_stamp, 15);
    put_le32(out + 128, metadata.capture_session_id);
    put_le32(out + CRC_OFFSET, crc32_ieee(out, CRC_OFFSET));
    return true;
}

bool EdfSessionMetadataCodec::inspect(
    const uint8_t *bytes,
    size_t length,
    EdfSessionMetadataFileInfo &info) {
    info = {};
    if (!bytes || length < 16 || memcmp(bytes, FILE_MAGIC, 8) != 0) {
        return false;
    }

    EdfSessionMetadataFileInfo parsed;
    parsed.version = get_le16(bytes + 8);
    parsed.header_bytes = get_le16(bytes + 10);
    parsed.total_bytes = get_le32(bytes + 12);
    if (parsed.version == 0 || parsed.header_bytes < 16 ||
        parsed.header_bytes > parsed.total_bytes ||
        parsed.total_bytes != length) {
        return false;
    }

    info = parsed;
    return true;
}

bool EdfSessionMetadataCodec::decode(const uint8_t *bytes,
                                     size_t length,
                                     EdfSessionMetadata &metadata) {
    EdfSessionMetadataFileInfo info;
    if (!inspect(bytes, length, info)) return false;

    switch (info.version) {
        case 1: return decode_v1(bytes, length, metadata);
        default: return false;
    }
}

uint64_t EdfSessionMetadataCodec::identity(const uint8_t *bytes,
                                           size_t length) {
    if (!bytes || length == 0) return 0;

    uint64_t hash = FNV_OFFSET;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash == 0 ? 1 : hash;
}

bool edf_session_metadata_path(const EdfSessionMetadata &metadata,
                               char *out,
                               size_t out_size) {
    if (!out || out_size == 0 || !edf_session_metadata_valid(metadata)) {
        return false;
    }

    const int written = snprintf(out,
                                 out_size,
                                 "%s/%s/%s.bin",
                                 EDF_SESSION_METADATA_ROOT,
                                 metadata.datalog_sleep_day,
                                 metadata.session_stamp);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool edf_session_metadata_path_matches(const char *path,
                                       const EdfSessionMetadata &metadata) {
    char expected[96] = {};
    return path && edf_session_metadata_path(metadata,
                                             expected,
                                             sizeof(expected)) &&
           strcmp(path, expected) == 0;
}

}  // namespace aircannect
