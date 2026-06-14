#include "report_store.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "can_datagram.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStore {
namespace {

constexpr const char *BASE_DIR = "/aircannect/report/v2";
constexpr uint32_t CHUNK_MAGIC = 0x50524341u;  // "ACRP", little-endian.
constexpr uint16_t CHUNK_SCHEMA = 3;  // bumped for chunk origin byte
constexpr size_t CHUNK_HEADER_SIZE = 56;
constexpr uint32_t SUMMARY_MAGIC = 0x53524341u;  // "ACRS", little-endian.
constexpr uint16_t SUMMARY_SCHEMA = 5;
constexpr size_t SUMMARY_HEADER_SIZE = 32;
constexpr size_t SUMMARY_RECORD_SIZE = 512;
constexpr size_t SUMMARY_SESSION_OFFSET = 64;
constexpr size_t SUMMARY_SESSION_SIZE = 12;
constexpr size_t SUMMARY_FIELD_OFFSET = 256;
constexpr size_t SUMMARY_FIELD_MASK_OFFSET = SUMMARY_FIELD_OFFSET;
constexpr size_t SUMMARY_FIELD_VALUE_OFFSET = SUMMARY_FIELD_OFFSET + 8;
static_assert(SUMMARY_FIELD_VALUE_OFFSET +
                      AC_REPORT_SUMMARY_FIELD_COUNT * sizeof(uint32_t) <=
                  SUMMARY_RECORD_SIZE,
              "summary field block must fit in summary record");
constexpr uint32_t COVERAGE_MAGIC = 0x56524341u;  // "ACRV", little-endian.
constexpr uint16_t COVERAGE_SCHEMA = 3;  // bumped: night-partitioned chunk layout
constexpr size_t COVERAGE_RECORD_SIZE = 48;
constexpr size_t COVERAGE_MAX_INTERVALS = 128;
constexpr uint32_t CHUNK_INDEX_MAGIC = 0x49524341u;  // "ACRI", little-endian.
constexpr uint16_t CHUNK_INDEX_SCHEMA = 2;  // bumped for chunk origin byte
constexpr size_t CHUNK_INDEX_RECORD_SIZE = 64;
constexpr size_t REPORT_PATH_MAX = 192;
constexpr const char *REPORT_TRASH_PREFIX = "v2-trash-";

ReportStoreStatus current;

void set_error(char *dst, size_t size, const char *error) {
    copy_cstr(dst, size, error);
}

void note_error(const char *error, uint32_t *counter) {
    if (counter) (*counter)++;
    set_error(current.last_error, sizeof(current.last_error), error);
}

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

int32_t float_to_milli(float value) {
    if (value >= 0.0f) {
        return static_cast<int32_t>(value * 1000.0f + 0.5f);
    }
    return static_cast<int32_t>(value * 1000.0f - 0.5f);
}

float milli_to_float(int32_t value) {
    return static_cast<float>(value) / 1000.0f;
}

bool valid_key(const ReportStoreChunkKey &key) {
    return key.source && key.source[0] && key.name && key.name[0] &&
           key.start_ms >= 0 && key.end_ms > key.start_ms;
}

bool valid_chunk_meta(const ReportStoreChunkMeta &meta) {
    return meta.payload_schema != 0;
}

void sanitize_name(const char *name, char *out, size_t out_size) {
    if (!out || !out_size) return;
    size_t pos = 0;
    for (const char *p = name ? name : ""; *p && pos + 1 < out_size; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (isalnum(ch) || ch == '-' || ch == '_') {
            out[pos++] = static_cast<char>(ch);
        } else {
            out[pos++] = '_';
        }
    }
    out[pos] = '\0';
}

uint32_t string_hash(const char *value) {
    return value && value[0]
        ? crc32_ieee(reinterpret_cast<const uint8_t *>(value), strlen(value))
        : crc32_ieee(nullptr, 0);
}

bool build_dir_path(const ReportStoreChunkKey &key,
                    char *path,
                    size_t path_size) {
    char safe_source[72];
    char safe_name[72];
    sanitize_name(key.source, safe_source, sizeof(safe_source));
    sanitize_name(key.name, safe_name, sizeof(safe_name));
    if (!safe_source[0] || !safe_name[0]) return false;
    int written;
    if (key.night_start_ms != 0) {
        written = snprintf(path, path_size, "%s/%s/%s/%s/%lld",
                           BASE_DIR, kind_name(key.kind),
                           safe_source, safe_name,
                           static_cast<long long>(key.night_start_ms));
    } else {
        written = snprintf(path, path_size, "%s/%s/%s/%s",
                           BASE_DIR, kind_name(key.kind),
                           safe_source, safe_name);
    }
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_source_dir_path(const ReportStoreChunkKey &key,
                           char *path,
                           size_t path_size) {
    char safe_source[72];
    sanitize_name(key.source, safe_source, sizeof(safe_source));
    if (!safe_source[0]) return false;
    const int written = snprintf(path, path_size, "%s/%s/%s",
                                 BASE_DIR, kind_name(key.kind),
                                 safe_source);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_chunk_path(const ReportStoreChunkKey &key,
                      char *path,
                      size_t path_size) {
    char dir[REPORT_PATH_MAX];
    if (!build_dir_path(key, dir, sizeof(dir))) return false;
    const int written = snprintf(path, path_size, "%s/%lld-%lld.bin",
                                 dir,
                                 static_cast<long long>(key.start_ms),
                                 static_cast<long long>(key.end_ms));
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_chunk_index_path(const ReportStoreChunkKey &key,
                            char *path,
                            size_t path_size) {
    char dir[REPORT_PATH_MAX];
    if (!build_dir_path(key, dir, sizeof(dir))) return false;
    const int written = snprintf(path, path_size, "%s/chunks.idx", dir);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_coverage_path(const char *source, char *path, size_t path_size) {
    char safe[72];
    sanitize_name(source, safe, sizeof(safe));
    if (!safe[0]) return false;
    const int written = snprintf(path, path_size, "%s/coverage/%s.idx",
                                 BASE_DIR, safe);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ensure_chunk_dir(const ReportStoreChunkKey &key) {
    char source_dir[REPORT_PATH_MAX];
    if (!build_source_dir_path(key, source_dir, sizeof(source_dir))) {
        note_error("bad_chunk_path", &current.layout_errors);
        return false;
    }
    if (!Storage::ensure_dir(source_dir)) {
        note_error("chunk_source_dir_failed", &current.layout_errors);
        return false;
    }

    // The partitioned path adds a <night_start_ms> dir under the name level, so
    // create the name level first.
    if (key.night_start_ms != 0) {
        ReportStoreChunkKey name_key = key;
        name_key.night_start_ms = 0;
        char name_dir[REPORT_PATH_MAX];
        if (!build_dir_path(name_key, name_dir, sizeof(name_dir))) {
            note_error("bad_chunk_path", &current.layout_errors);
            return false;
        }
        if (!Storage::ensure_dir(name_dir)) {
            note_error("chunk_dir_failed", &current.layout_errors);
            return false;
        }
    }

    char dir[REPORT_PATH_MAX];
    if (!build_dir_path(key, dir, sizeof(dir))) {
        note_error("bad_chunk_path", &current.layout_errors);
        return false;
    }
    if (!Storage::ensure_dir(dir)) {
        note_error("chunk_dir_failed", &current.layout_errors);
        return false;
    }
    return true;
}

bool write_all(File &file, const uint8_t *data, size_t len) {
    if (!len) return true;
    return data && file.write(data, len) == len;
}

bool read_all(File &file, uint8_t *data, size_t len) {
    if (!len) return true;
    return data && file.read(data, len) == static_cast<int>(len);
}

void encode_summary_header(uint8_t *header,
                           uint32_t record_count,
                           uint32_t payload_len,
                           uint32_t payload_crc) {
    memset(header, 0, SUMMARY_HEADER_SIZE);
    put_le32(header + 0, SUMMARY_MAGIC);
    put_le16(header + 4, SUMMARY_SCHEMA);
    put_le16(header + 6, SUMMARY_HEADER_SIZE);
    put_le16(header + 8, SUMMARY_RECORD_SIZE);
    put_le32(header + 12, record_count);
    put_le32(header + 16, payload_len);
    put_le32(header + 20, payload_crc);
}

bool decode_summary_header(const uint8_t *header,
                           uint32_t &record_count,
                           uint32_t &payload_len,
                           uint32_t &payload_crc) {
    if (get_le32(header + 0) != SUMMARY_MAGIC) return false;
    if (get_le16(header + 4) != SUMMARY_SCHEMA) return false;
    if (get_le16(header + 6) != SUMMARY_HEADER_SIZE) return false;
    if (get_le16(header + 8) != SUMMARY_RECORD_SIZE) return false;
    record_count = get_le32(header + 12);
    payload_len = get_le32(header + 16);
    payload_crc = get_le32(header + 20);
    return payload_len == record_count * SUMMARY_RECORD_SIZE;
}

void encode_summary_record(uint8_t *raw,
                           const ReportSummaryRecord &record) {
    memset(raw, 0, SUMMARY_RECORD_SIZE);
    uint32_t flags = 0;
    if (record.valid) flags |= 1u << 0;
    if (record.has_tz_offset_min) flags |= 1u << 1;
    if (record.has_ahi) flags |= 1u << 2;
    if (record.has_apnea_index) flags |= 1u << 3;
    if (record.has_hypopnea_index) flags |= 1u << 4;
    if (record.has_oa_index) flags |= 1u << 5;
    if (record.has_ca_index) flags |= 1u << 6;
    if (record.has_ua_index) flags |= 1u << 7;
    if (record.has_rera_index) flags |= 1u << 8;
    if (record.has_session_count) flags |= 1u << 9;
    if (record.session_interval_count > 0) flags |= 1u << 10;
    if (record.summary_field_mask != 0) flags |= 1u << 11;

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
    put_le32(raw + 60, record.session_interval_count);

    const uint32_t session_count =
        record.session_interval_count < AC_REPORT_SUMMARY_SESSION_MAX
            ? record.session_interval_count
            : AC_REPORT_SUMMARY_SESSION_MAX;
    for (uint32_t i = 0; i < session_count; ++i) {
        uint8_t *session = raw + SUMMARY_SESSION_OFFSET +
                           i * SUMMARY_SESSION_SIZE;
        put_le64(session + 0, record.sessions[i].start_ms);
        put_le32(session + 8, record.sessions[i].duration_min);
    }

    put_le64(raw + SUMMARY_FIELD_MASK_OFFSET, record.summary_field_mask);
    for (size_t i = 0; i < AC_REPORT_SUMMARY_FIELD_COUNT; ++i) {
        const size_t offset = SUMMARY_FIELD_VALUE_OFFSET + i * 4;
        put_le32(raw + offset, record.summary_field_values[i]);
    }
}

bool decode_summary_record(const uint8_t *raw,
                           ReportSummaryRecord &record) {
    const uint32_t flags = get_le32(raw + 0);
    record = {};
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
    const bool has_summary_fields = (flags & (1u << 11)) != 0;

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
    uint32_t session_count = get_le32(raw + 60);
    if (session_count > AC_REPORT_SUMMARY_SESSION_MAX) {
        session_count = AC_REPORT_SUMMARY_SESSION_MAX;
    }
    for (uint32_t i = 0; i < session_count; ++i) {
        const uint8_t *session = raw + SUMMARY_SESSION_OFFSET +
                                 i * SUMMARY_SESSION_SIZE;
        const uint64_t start_ms = get_le64(session + 0);
        const uint32_t duration_min = get_le32(session + 8);
        if (!start_ms || !duration_min) continue;
        ReportSummarySession &out_session =
            record.sessions[record.session_interval_count++];
        out_session.start_ms = start_ms;
        out_session.duration_min = duration_min;
    }
    if (!record.has_session_count && record.session_interval_count > 0) {
        record.has_session_count = true;
        record.session_count = record.session_interval_count;
    }

    if (has_summary_fields) {
        record.summary_field_mask = get_le64(raw + SUMMARY_FIELD_MASK_OFFSET);
        for (size_t i = 0; i < AC_REPORT_SUMMARY_FIELD_COUNT; ++i) {
            const size_t offset = SUMMARY_FIELD_VALUE_OFFSET + i * 4;
            record.summary_field_values[i] = get_le32(raw + offset);
        }
    }
    return record.valid && record.start_ms > 0 &&
           record.end_ms > record.start_ms;
}

bool append_summary_record(ReportSpoolBuffer &payload,
                           const ReportSummaryRecord &record) {
    size_t offset = 0;
    uint8_t *raw = payload.append_uninitialized(SUMMARY_RECORD_SIZE, offset);
    if (!raw) return false;
    encode_summary_record(raw, record);
    return true;
}

void encode_header(uint8_t *header,
                   const ReportStoreChunkKey &key,
                   const ReportStoreChunkMeta &meta,
                   size_t payload_len,
                   uint32_t payload_crc) {
    memset(header, 0, CHUNK_HEADER_SIZE);
    put_le32(header + 0, CHUNK_MAGIC);
    put_le16(header + 4, CHUNK_SCHEMA);
    put_le16(header + 6, CHUNK_HEADER_SIZE);
    header[8] = static_cast<uint8_t>(key.kind);
    header[9] = static_cast<uint8_t>(key.origin);
    put_le64(header + 12, static_cast<uint64_t>(key.start_ms));
    put_le64(header + 20, static_cast<uint64_t>(key.end_ms));
    put_le32(header + 28, meta.payload_schema);
    put_le32(header + 32, meta.record_count);
    put_le32(header + 36, static_cast<uint32_t>(payload_len));
    put_le32(header + 40, payload_crc);
    put_le32(header + 44, string_hash(key.source));
    put_le32(header + 48, string_hash(key.name));
}

bool decode_header(const uint8_t *header,
                   const ReportStoreChunkKey &key,
                   ReportStoreChunkMeta &meta,
                   uint32_t &payload_len,
                   uint32_t &payload_crc) {
    if (get_le32(header + 0) != CHUNK_MAGIC) return false;
    if (get_le16(header + 4) != CHUNK_SCHEMA) return false;
    if (get_le16(header + 6) != CHUNK_HEADER_SIZE) return false;
    if (header[8] != static_cast<uint8_t>(key.kind)) return false;
    if (static_cast<int64_t>(get_le64(header + 12)) != key.start_ms) {
        return false;
    }
    if (static_cast<int64_t>(get_le64(header + 20)) != key.end_ms) {
        return false;
    }
    meta.payload_schema = get_le32(header + 28);
    meta.record_count = get_le32(header + 32);
    payload_len = get_le32(header + 36);
    payload_crc = get_le32(header + 40);
    if (get_le32(header + 44) != string_hash(key.source)) return false;
    if (get_le32(header + 48) != string_hash(key.name)) return false;
    return valid_chunk_meta(meta);
}

void encode_chunk_index_record(uint8_t *raw,
                               const ReportStoreChunkKey &key,
                               const ReportStoreChunkMeta &meta,
                               uint32_t payload_len) {
    memset(raw, 0, CHUNK_INDEX_RECORD_SIZE);
    put_le32(raw + 0, CHUNK_INDEX_MAGIC);
    put_le16(raw + 4, CHUNK_INDEX_SCHEMA);
    put_le16(raw + 6, CHUNK_INDEX_RECORD_SIZE);
    raw[8] = static_cast<uint8_t>(key.kind);
    raw[9] = static_cast<uint8_t>(key.origin);
    put_le32(raw + 12, string_hash(key.source));
    put_le32(raw + 16, string_hash(key.name));
    put_le64(raw + 20, static_cast<uint64_t>(key.start_ms));
    put_le64(raw + 28, static_cast<uint64_t>(key.end_ms));
    put_le32(raw + 36, meta.payload_schema);
    put_le32(raw + 40, meta.record_count);
    put_le32(raw + 44, payload_len);
    put_le32(raw + 60, crc32_ieee(raw, CHUNK_INDEX_RECORD_SIZE - 4));
}

bool decode_chunk_index_record(const uint8_t *raw,
                               const ReportStoreChunkKey &query,
                               ReportStoreChunkInfo &info) {
    if (get_le32(raw + 0) != CHUNK_INDEX_MAGIC) return false;
    if (get_le16(raw + 4) != CHUNK_INDEX_SCHEMA) return false;
    if (get_le16(raw + 6) != CHUNK_INDEX_RECORD_SIZE) return false;
    if (crc32_ieee(raw, CHUNK_INDEX_RECORD_SIZE - 4) !=
        get_le32(raw + 60)) {
        return false;
    }
    if (raw[8] != static_cast<uint8_t>(query.kind)) return false;
    if (get_le32(raw + 12) != string_hash(query.source)) return false;
    if (get_le32(raw + 16) != string_hash(query.name)) return false;

    info = {};
    info.key.kind = query.kind;
    info.key.source = query.source;
    info.key.name = query.name;
    info.key.origin = static_cast<ReportStoreChunkOrigin>(raw[9]);
    info.key.start_ms = static_cast<int64_t>(get_le64(raw + 20));
    info.key.end_ms = static_cast<int64_t>(get_le64(raw + 28));
    info.meta.payload_schema = get_le32(raw + 36);
    info.meta.record_count = get_le32(raw + 40);
    info.payload_len = get_le32(raw + 44);
    return valid_key(info.key) && valid_chunk_meta(info.meta);
}

bool chunk_index_contains(const ReportStoreChunkKey &key,
                          const ReportStoreChunkMeta &meta,
                          uint32_t payload_len) {
    char path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(key, path, sizeof(path)) ||
        !Storage::exists(path)) {
        return false;
    }

    File file = Storage::open(path, "r");
    if (!file) return false;

    bool found = false;
    uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
    while (file.read(raw, sizeof(raw)) == static_cast<int>(sizeof(raw))) {
        ReportStoreChunkInfo info;
        if (!decode_chunk_index_record(raw, key, info)) continue;
        if (info.key.start_ms == key.start_ms &&
            info.key.end_ms == key.end_ms &&
            info.meta.payload_schema == meta.payload_schema &&
            info.meta.record_count == meta.record_count &&
            info.payload_len == payload_len) {
            found = true;
            break;
        }
    }
    file.close();
    return found;
}

bool ensure_chunk_index_record(const ReportStoreChunkKey &key,
                               const ReportStoreChunkMeta &meta,
                               uint32_t payload_len) {
    if (chunk_index_contains(key, meta, payload_len)) return true;

    char path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(key, path, sizeof(path))) {
        note_error("bad_chunk_index_path", &current.write_errors);
        return false;
    }

    File file = Storage::open(path, "a");
    if (!file) {
        note_error("chunk_index_open_failed", &current.write_errors);
        return false;
    }
    uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
    encode_chunk_index_record(raw, key, meta, payload_len);
    const bool ok = write_all(file, raw, sizeof(raw));
    file.close();
    if (!ok) {
        note_error("chunk_index_write_failed", &current.write_errors);
        return false;
    }
    return true;
}

bool parse_int64_until(const char *&cursor,
                       char delimiter,
                       int64_t &out) {
    if (!cursor || *cursor < '0' || *cursor > '9') return false;
    int64_t value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        const int digit = *cursor - '0';
        if (value > (INT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++cursor;
    }
    if (*cursor != delimiter) return false;
    ++cursor;
    out = value;
    return true;
}

bool parse_chunk_filename(const char *name,
                          int64_t &start_ms,
                          int64_t &end_ms) {
    if (!name || !*name) return false;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *cursor = base;
    if (!parse_int64_until(cursor, '-', start_ms)) return false;
    if (!parse_int64_until(cursor, '.', end_ms)) return false;
    return strcmp(cursor, "bin") == 0 && end_ms > start_ms;
}

const char *path_basename(const char *path) {
    if (!path) return "";
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

bool build_child_path(const char *parent,
                      const char *child_name,
                      char *out,
                      size_t out_size) {
    if (!parent || !parent[0] || !child_name || !child_name[0] ||
        !out || !out_size) {
        return false;
    }
    if (child_name[0] == '/') {
        const int written = snprintf(out, out_size, "%s", child_name);
        return written > 0 && static_cast<size_t>(written) < out_size;
    }
    const int written = snprintf(out, out_size, "%s/%s", parent, child_name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool is_report_trash_dir_name(const char *name) {
    const char *base = path_basename(name);
    return strncmp(base,
                   REPORT_TRASH_PREFIX,
                   strlen(REPORT_TRASH_PREFIX)) == 0;
}

bool ranges_overlap(int64_t left_start,
                    int64_t left_end,
                    int64_t right_start,
                    int64_t right_end) {
    return left_end > right_start && left_start < right_end;
}

bool valid_coverage_record(const ReportStoreCoverageRecord &record) {
    return record.start_ms >= 0 && record.end_ms > record.start_ms &&
           record.parser_schema != 0 &&
           (record.state == ReportStoreCoverageState::Complete ||
            record.state == ReportStoreCoverageState::Incomplete);
}

void encode_coverage_record(uint8_t *raw,
                            const ReportStoreCoverageRecord &record) {
    memset(raw, 0, COVERAGE_RECORD_SIZE);
    put_le32(raw + 0, COVERAGE_MAGIC);
    put_le16(raw + 4, COVERAGE_SCHEMA);
    put_le16(raw + 6, COVERAGE_RECORD_SIZE);
    raw[8] = static_cast<uint8_t>(record.state);
    raw[9] = static_cast<uint8_t>(record.origin);
    put_le16(raw + 10, record.error_code);
    put_le32(raw + 12, record.parser_schema);
    put_le32(raw + 16, record.source_hash);
    put_le64(raw + 24, static_cast<uint64_t>(record.start_ms));
    put_le64(raw + 32, static_cast<uint64_t>(record.end_ms));
    put_le32(raw + 40, millis());
    put_le32(raw + 44, crc32_ieee(raw, COVERAGE_RECORD_SIZE - 4));
}

bool decode_coverage_record(const uint8_t *raw,
                            ReportStoreCoverageRecord &record) {
    if (get_le32(raw + 0) != COVERAGE_MAGIC) return false;
    if (get_le16(raw + 4) != COVERAGE_SCHEMA) return false;
    if (get_le16(raw + 6) != COVERAGE_RECORD_SIZE) return false;
    const uint32_t expected_crc = get_le32(raw + 44);
    if (crc32_ieee(raw, COVERAGE_RECORD_SIZE - 4) != expected_crc) {
        return false;
    }

    record = {};
    record.state = static_cast<ReportStoreCoverageState>(raw[8]);
    record.origin = static_cast<ReportStoreChunkOrigin>(raw[9]);
    record.error_code = get_le16(raw + 10);
    record.parser_schema = get_le32(raw + 12);
    record.source_hash = get_le32(raw + 16);
    record.start_ms = static_cast<int64_t>(get_le64(raw + 24));
    record.end_ms = static_cast<int64_t>(get_le64(raw + 32));
    return valid_coverage_record(record);
}

bool read_coverage_record(File &file,
                          ReportStoreCoverageRecord &record,
                          bool &eof) {
    eof = false;
    uint8_t raw[COVERAGE_RECORD_SIZE];
    const int got = file.read(raw, sizeof(raw));
    if (got <= 0) {
        eof = true;
        return false;
    }
    if (got != static_cast<int>(sizeof(raw))) {
        eof = true;  // truncated tail: can't resync on a fixed-size stream, stop
        return false;
    }
    // false here (full record, bad magic/schema/CRC) is a skippable stale record
    return decode_coverage_record(raw, record);
}

// Coverage is stored per source as Complete intervals kept sorted by start and
// merged when same-tag (parser_schema/source_hash/origin) spans overlap or sit
// within the join tolerance; "missing" is simply a gap. A query is one walk for
// the first gap; a write loads, merges, and atomically rewrites, so refreshing a
// covered span never grows the file and old append-log files coalesce on first
// read. coverage_scratch is reused under the SD guard, which serialises every
// coverage op, so a single shared buffer is safe. It is lazy/PSRAM-backed:
// report coverage is storage/report work, not a CAN timing path.
ReportStoreCoverageRecord *coverage_scratch = nullptr;

ReportStoreCoverageRecord *ensure_coverage_scratch() {
    if (coverage_scratch) return coverage_scratch;
    coverage_scratch = static_cast<ReportStoreCoverageRecord *>(
        Memory::calloc_large(COVERAGE_MAX_INTERVALS,
                             sizeof(ReportStoreCoverageRecord)));
    return coverage_scratch;
}

bool coverage_same_tag(const ReportStoreCoverageRecord &a,
                       const ReportStoreCoverageRecord &b) {
    return a.parser_schema == b.parser_schema &&
           a.source_hash == b.source_hash && a.origin == b.origin;
}

// Insert rec into recs[] (kept sorted by start) and coalesce the same-tag run it
// now belongs to. Only Complete intervals are tracked; on overflow the interval
// is dropped (its span reads as missing and is re-fetched).
void coverage_insert(ReportStoreCoverageRecord *recs, size_t &count,
                     const ReportStoreCoverageRecord &rec) {
    if (rec.state != ReportStoreCoverageState::Complete ||
        rec.end_ms <= rec.start_ms) {
        return;
    }
    if (count >= COVERAGE_MAX_INTERVALS) return;
    size_t pos = 0;
    while (pos < count && recs[pos].start_ms < rec.start_ms) ++pos;
    for (size_t i = count; i > pos; --i) recs[i] = recs[i - 1];
    recs[pos] = rec;
    ++count;
    // One left-to-right pass coalesces adjacent same-tag spans within tolerance.
    // It suffices because the array is sorted and a source has a single tag in
    // practice, so the whole run merges transitively (and any leftover adjacency
    // is still stitched by the query walk).
    const int64_t tol = AC_REPORT_COVERAGE_TOLERANCE_MS;
    size_t w = 0;
    for (size_t r = 1; r < count; ++r) {
        if (coverage_same_tag(recs[w], recs[r]) &&
            recs[r].start_ms <= recs[w].end_ms + tol) {
            if (recs[r].end_ms > recs[w].end_ms) recs[w].end_ms = recs[r].end_ms;
        } else {
            recs[++w] = recs[r];
        }
    }
    count = w + 1;
}

// Read a source's coverage file into recs[], coalescing as it goes; returns the
// interval count. Stale/old-schema records are skipped.
size_t load_coverage(const char *source, ReportStoreCoverageRecord *recs) {
    size_t count = 0;
    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) return 0;
    if (!Storage::exists(path)) return 0;
    File file = Storage::open(path, "r");
    if (!file) return 0;
    for (;;) {
        bool eof = false;
        ReportStoreCoverageRecord record;
        if (!read_coverage_record(file, record, eof)) {
            if (eof) break;
            current.coverage_read_errors++;  // skippable stale record
            continue;
        }
        current.coverage_records_read++;
        coverage_insert(recs, count, record);
    }
    file.close();
    return count;
}

// Atomically replace a source's coverage file with recs[] (tmp + rename); an
// empty set removes the file.
bool rewrite_coverage_file(const char *source,
                           const ReportStoreCoverageRecord *recs,
                           size_t count) {
    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) return false;
    char tmp[REPORT_PATH_MAX + 8];
    const int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;
    Storage::remove(tmp);
    File out = Storage::open(tmp, "w");
    if (!out) return false;
    bool ok = true;
    for (size_t i = 0; i < count && ok; ++i) {
        uint8_t raw[COVERAGE_RECORD_SIZE];
        encode_coverage_record(raw, recs[i]);
        ok = write_all(out, raw, sizeof(raw));
    }
    out.close();
    if (!ok) {
        Storage::remove(tmp);
        return false;
    }
    Storage::remove(path);
    if (count == 0) {
        Storage::remove(tmp);
        return true;
    }
    if (!Storage::rename(tmp, path)) {
        Storage::remove(tmp);
        return false;
    }
    return true;
}

}  // namespace

void begin() {
    Storage::Guard g;
    if (current.initialized) return;
    current.initialized = true;
    ensure_layout();
}

ReportStoreStatus status() {
    if (!current.initialized) begin();
    current.available = Storage::mounted();
    return current;
}

bool ready() {
    if (!current.initialized) begin();
    return current.available;
}

bool ensure_layout() {
    Storage::Guard g;
    current.available = Storage::mounted();
    if (!current.available) {
        note_error("storage_unavailable", &current.layout_errors);
        return false;
    }

    const char *dirs[] = {
        "/aircannect",
        "/aircannect/report",
        BASE_DIR,
        "/aircannect/report/v2/summary",
        "/aircannect/report/v2/coverage",
        "/aircannect/report/v2/series",
        "/aircannect/report/v2/events",
    };
    for (const char *dir : dirs) {
        if (!Storage::ensure_dir(dir)) {
            note_error("layout_dir_failed", &current.layout_errors);
            return false;
        }
    }
    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

bool chunk_exists(const ReportStoreChunkKey &key) {
    Storage::Guard g;
    if (!valid_key(key)) return false;
    char path[REPORT_PATH_MAX];
    return build_chunk_path(key, path, sizeof(path)) && Storage::exists(path);
}

bool write_chunk(const ReportStoreChunkKey &key,
                 const ReportStoreChunkMeta &meta,
                 const uint8_t *payload,
                 size_t len) {
    Storage::Guard g;
    if (!valid_key(key)) {
        note_error("bad_chunk_key", &current.write_errors);
        return false;
    }
    if (!valid_chunk_meta(meta)) {
        note_error("bad_chunk_meta", &current.write_errors);
        return false;
    }
    if (len > UINT32_MAX) {
        note_error("chunk_too_large", &current.write_errors);
        return false;
    }
    if (!ensure_layout() || !ensure_chunk_dir(key)) return false;

    char final_path[REPORT_PATH_MAX];
    char tmp_path[REPORT_PATH_MAX + 8];
    if (!build_chunk_path(key, final_path, sizeof(final_path))) {
        note_error("bad_chunk_path", &current.write_errors);
        return false;
    }
    if (Storage::exists(final_path)) {
        return ensure_chunk_index_record(
            key, meta, static_cast<uint32_t>(len));
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    Storage::remove(tmp_path);

    File file = Storage::open(tmp_path, "w");
    if (!file) {
        note_error("chunk_open_failed", &current.write_errors);
        return false;
    }

    uint8_t header[CHUNK_HEADER_SIZE];
    const uint32_t crc = len ? crc32_ieee(payload, len) : crc32_ieee(nullptr, 0);
    encode_header(header, key, meta, len, crc);
    bool ok = write_all(file, header, sizeof(header)) &&
              write_all(file, payload, len);
    file.close();

    if (!ok) {
        Storage::remove(tmp_path);
        note_error("chunk_write_failed", &current.write_errors);
        return false;
    }

    if (!Storage::rename(tmp_path, final_path)) {
        Storage::remove(tmp_path);
        note_error("chunk_commit_failed", &current.write_errors);
        return false;
    }

    current.chunks_written++;
    current.bytes_written += len;
    if (!ensure_chunk_index_record(key, meta, static_cast<uint32_t>(len))) {
        return false;
    }
    set_error(current.last_error, sizeof(current.last_error), "");
    Log::logf(CAT_RPC, LOG_DEBUG,
              "[REPORT] stored chunk kind=%s source=%s name=%s "
              "start=%lld end=%lld schema=%lu bytes=%lu records=%lu\n",
              kind_name(key.kind), key.source, key.name,
              static_cast<long long>(key.start_ms),
              static_cast<long long>(key.end_ms),
              static_cast<unsigned long>(meta.payload_schema),
              static_cast<unsigned long>(len),
              static_cast<unsigned long>(meta.record_count));
    return true;
}

bool read_chunk(const ReportStoreChunkKey &key,
                ReportStoreChunkMeta &meta,
                ReportSpoolBuffer &payload) {
    Storage::Guard g;
    if (!valid_key(key)) {
        note_error("bad_chunk_key", &current.read_errors);
        return false;
    }
    char path[REPORT_PATH_MAX];
    if (!build_chunk_path(key, path, sizeof(path))) {
        note_error("bad_chunk_path", &current.read_errors);
        return false;
    }
    File file = Storage::open(path, "r");
    if (!file) {
        note_error("chunk_open_failed", &current.read_errors);
        return false;
    }

    uint8_t header[CHUNK_HEADER_SIZE];
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
    bool ok = read_all(file, header, sizeof(header)) &&
              decode_header(header, key, meta, payload_len, payload_crc);
    if (ok && payload_len && !payload.reserve_capacity(payload_len)) {
        ok = false;
    }
    if (ok && payload_len) {
        size_t offset = 0;
        uint8_t *dst = payload.append_uninitialized(payload_len, offset);
        ok = dst && read_all(file, dst, payload_len);
    }
    file.close();

    if (!ok) {
        payload.clear();
        note_error("chunk_read_failed", &current.read_errors);
        return false;
    }
    const uint32_t actual_crc = payload.size()
        ? crc32_ieee(payload.data(), payload.size())
        : crc32_ieee(nullptr, 0);
    if (actual_crc != payload_crc) {
        payload.clear();
        note_error("chunk_crc_failed", &current.read_errors);
        return false;
    }

    current.chunks_read++;
    current.bytes_read += payload.size();
    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

bool for_each_chunk(ReportStoreChunkKind kind,
                    const char *source,
                    const char *name,
                    int64_t night_start_ms,
                    int64_t start_ms,
                    int64_t end_ms,
                    ReportStoreChunkCallback callback,
                    void *context) {
    Storage::Guard g;
    ReportStoreChunkKey dir_key;
    dir_key.kind = kind;
    dir_key.source = source;
    dir_key.name = name;
    dir_key.start_ms = 0;
    dir_key.end_ms = 1;
    dir_key.night_start_ms = night_start_ms;
    if (!valid_key(dir_key) || start_ms < 0 || end_ms <= start_ms ||
        !callback) {
        note_error("bad_chunk_query", &current.read_errors);
        return false;
    }

    char dir_path[REPORT_PATH_MAX];
    if (!build_dir_path(dir_key, dir_path, sizeof(dir_path))) {
        note_error("bad_chunk_path", &current.read_errors);
        return false;
    }

    char index_path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(dir_key, index_path, sizeof(index_path))) {
        note_error("bad_chunk_index_path", &current.read_errors);
        return false;
    }
    if (Storage::exists(index_path)) {
        File index = Storage::open(index_path, "r");
        if (!index) {
            note_error("chunk_index_open_failed", &current.read_errors);
            return false;
        }

        bool ok = true;
        while (true) {
            uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
            const int got = index.read(raw, sizeof(raw));
            if (got == 0) break;
            if (got != static_cast<int>(sizeof(raw))) {
                ok = false;
                note_error("chunk_index_short_read", &current.read_errors);
                break;
            }
            ReportStoreChunkInfo info;
            if (!decode_chunk_index_record(raw, dir_key, info)) {
                ok = false;
                note_error("chunk_index_bad_record", &current.read_errors);
                break;
            }
            if (!ranges_overlap(info.key.start_ms,
                                info.key.end_ms,
                                start_ms,
                                end_ms)) {
                continue;
            }
            current.chunks_listed++;
            if (!callback(context, info)) {
                ok = false;
                note_error("chunk_callback_rejected", &current.read_errors);
                break;
            }
        }
        index.close();
        if (ok) set_error(current.last_error, sizeof(current.last_error), "");
        return ok;
    }

    File dir = Storage::open(dir_path, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        note_error("chunk_path_not_dir", &current.read_errors);
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        if (file.isDirectory()) {
            file.close();
            continue;
        }

        int64_t chunk_start = 0;
        int64_t chunk_end = 0;
        if (!parse_chunk_filename(file.name(), chunk_start, chunk_end) ||
            !ranges_overlap(chunk_start, chunk_end, start_ms, end_ms)) {
            file.close();
            continue;
        }

        ReportStoreChunkKey key;
        key.kind = kind;
        key.source = source;
        key.name = name;
        key.start_ms = chunk_start;
        key.end_ms = chunk_end;

        uint8_t header[CHUNK_HEADER_SIZE];
        ReportStoreChunkInfo info;
        uint32_t payload_crc = 0;
        if (!read_all(file, header, sizeof(header)) ||
            !decode_header(header,
                           key,
                           info.meta,
                           info.payload_len,
                           payload_crc)) {
            ok = false;
            file.close();
            note_error("chunk_header_failed", &current.read_errors);
            break;
        }
        info.key = key;
        if (!ensure_chunk_index_record(key, info.meta, info.payload_len)) {
            ok = false;
            file.close();
            break;
        }
        current.chunks_listed++;
        if (!callback(context, info)) {
            ok = false;
            file.close();
            note_error("chunk_callback_rejected", &current.read_errors);
            break;
        }
        file.close();
    }
    dir.close();
    if (ok) set_error(current.last_error, sizeof(current.last_error), "");
    return ok;
}

bool clear_chunks(ReportStoreChunkKind kind,
                  const char *source,
                  const char *name,
                  int64_t night_start_ms,
                  int64_t start_ms,
                  int64_t end_ms,
                  uint32_t &deleted) {
    Storage::Guard g;
    deleted = 0;
    ReportStoreChunkKey dir_key;
    dir_key.kind = kind;
    dir_key.source = source;
    dir_key.name = name;
    dir_key.start_ms = 0;
    dir_key.end_ms = 1;
    dir_key.night_start_ms = night_start_ms;
    if (!valid_key(dir_key) || start_ms < 0 || end_ms <= start_ms) {
        note_error("bad_chunk_clear_query", &current.write_errors);
        return false;
    }

    char dir_path[REPORT_PATH_MAX];
    if (!build_dir_path(dir_key, dir_path, sizeof(dir_path))) {
        note_error("bad_chunk_path", &current.write_errors);
        return false;
    }

    char index_path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(dir_key, index_path, sizeof(index_path))) {
        note_error("bad_chunk_index_path", &current.write_errors);
        return false;
    }

    if (Storage::exists(index_path)) {
        char tmp_path[REPORT_PATH_MAX + 8];
        const int tmp_written =
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", index_path);
        if (tmp_written <= 0 ||
            static_cast<size_t>(tmp_written) >= sizeof(tmp_path)) {
            note_error("bad_chunk_index_tmp", &current.write_errors);
            return false;
        }
        Storage::remove(tmp_path);

        File index = Storage::open(index_path, "r");
        File tmp = Storage::open(tmp_path, "w");
        if (!index || !tmp) {
            if (index) index.close();
            if (tmp) tmp.close();
            Storage::remove(tmp_path);
            note_error("chunk_index_clear_open_failed",
                       &current.write_errors);
            return false;
        }

        bool ok = true;
        bool kept = false;
        while (true) {
            uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
            const int got = index.read(raw, sizeof(raw));
            if (got == 0) break;
            if (got != static_cast<int>(sizeof(raw))) {
                ok = false;
                note_error("chunk_index_short_read", &current.write_errors);
                break;
            }
            ReportStoreChunkInfo info;
            if (!decode_chunk_index_record(raw, dir_key, info)) {
                ok = false;
                note_error("chunk_index_bad_record", &current.write_errors);
                break;
            }
            if (ranges_overlap(info.key.start_ms,
                               info.key.end_ms,
                               start_ms,
                               end_ms)) {
                char chunk_path[REPORT_PATH_MAX];
                if (!build_chunk_path(info.key,
                                      chunk_path,
                                      sizeof(chunk_path))) {
                    ok = false;
                    note_error("bad_chunk_path", &current.write_errors);
                    break;
                }
                const bool existed = Storage::exists(chunk_path);
                if (!Storage::remove(chunk_path)) {
                    ok = false;
                    note_error("chunk_remove_failed", &current.write_errors);
                    break;
                }
                if (existed) deleted++;
                continue;
            }
            if (!write_all(tmp, raw, sizeof(raw))) {
                ok = false;
                note_error("chunk_index_rewrite_failed",
                           &current.write_errors);
                break;
            }
            kept = true;
        }
        index.close();
        tmp.close();

        if (!ok) {
            Storage::remove(tmp_path);
            return false;
        }
        if (!Storage::remove(index_path)) {
            Storage::remove(tmp_path);
            note_error("chunk_index_remove_failed", &current.write_errors);
            return false;
        }
        if (kept) {
            if (!Storage::rename(tmp_path, index_path)) {
                Storage::remove(tmp_path);
                note_error("chunk_index_commit_failed",
                           &current.write_errors);
                return false;
            }
        } else {
            Storage::remove(tmp_path);
        }
        set_error(current.last_error, sizeof(current.last_error), "");
        return true;
    }

    File dir = Storage::open(dir_path, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        note_error("chunk_path_not_dir", &current.write_errors);
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        if (file.isDirectory()) {
            file.close();
            continue;
        }
        int64_t chunk_start = 0;
        int64_t chunk_end = 0;
        if (!parse_chunk_filename(file.name(), chunk_start, chunk_end) ||
            !ranges_overlap(chunk_start, chunk_end, start_ms, end_ms)) {
            file.close();
            continue;
        }
        ReportStoreChunkKey key;
        key.kind = kind;
        key.source = source;
        key.name = name;
        key.start_ms = chunk_start;
        key.end_ms = chunk_end;
        char chunk_path[REPORT_PATH_MAX];
        const bool path_ok =
            build_chunk_path(key, chunk_path, sizeof(chunk_path));
        file.close();
        if (!path_ok || !Storage::remove(chunk_path)) {
            ok = false;
            note_error("chunk_remove_failed", &current.write_errors);
            break;
        }
        deleted++;
    }
    dir.close();
    if (ok) set_error(current.last_error, sizeof(current.last_error), "");
    return ok;
}

bool write_summary_records(const ReportSummaryRecord *records,
                           size_t count) {
    Storage::Guard g;
    if (!records && count) {
        note_error("bad_summary_records", &current.write_errors);
        return false;
    }
    if (count > UINT32_MAX ||
        count > (AC_REPORT_MAX_PAYLOAD_BYTES / SUMMARY_RECORD_SIZE)) {
        note_error("summary_too_large", &current.write_errors);
        return false;
    }
    if (!ensure_layout()) return false;

    ReportSpoolBuffer payload;
    payload.set_max_size(AC_REPORT_MAX_PAYLOAD_BYTES);
    for (size_t i = 0; i < count; ++i) {
        if (!append_summary_record(payload, records[i])) {
            note_error("summary_encode_failed", &current.write_errors);
            return false;
        }
    }

    char final_path[REPORT_PATH_MAX];
    char tmp_path[REPORT_PATH_MAX];
    snprintf(final_path, sizeof(final_path), "%s/summary/nights.idx",
             BASE_DIR);
    snprintf(tmp_path, sizeof(tmp_path), "%s/summary/nights.idx.tmp",
             BASE_DIR);
    Storage::remove(tmp_path);
    File file = Storage::open(tmp_path, "w");
    if (!file) {
        note_error("summary_open_failed", &current.write_errors);
        return false;
    }

    uint8_t header[SUMMARY_HEADER_SIZE];
    const uint32_t crc = payload.size()
        ? crc32_ieee(payload.data(), payload.size())
        : crc32_ieee(nullptr, 0);
    encode_summary_header(header,
                          static_cast<uint32_t>(count),
                          static_cast<uint32_t>(payload.size()),
                          crc);
    bool ok = write_all(file, header, sizeof(header)) &&
              write_all(file, payload.data(), payload.size());
    file.close();
    if (!ok) {
        Storage::remove(tmp_path);
        note_error("summary_write_failed", &current.write_errors);
        return false;
    }
    Storage::remove(final_path);
    if (!Storage::rename(tmp_path, final_path)) {
        Storage::remove(tmp_path);
        note_error("summary_commit_failed", &current.write_errors);
        return false;
    }

    current.summary_records_written += static_cast<uint32_t>(count);
    current.bytes_written += payload.size();
    set_error(current.last_error, sizeof(current.last_error), "");
    Log::logf(CAT_RPC, LOG_DEBUG,
              "[REPORT] stored summary records=%lu bytes=%lu\n",
              static_cast<unsigned long>(count),
              static_cast<unsigned long>(payload.size()));
    return true;
}

bool read_summary_records(ReportSummaryRecordCallback callback,
                          void *context) {
    Storage::Guard g;
    if (!callback) {
        note_error("missing_summary_callback", &current.read_errors);
        return false;
    }
    char path[REPORT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/summary/nights.idx", BASE_DIR);
    if (!ensure_layout() || !Storage::exists(path)) return false;

    File file = Storage::open(path, "r");
    if (!file) {
        note_error("summary_open_failed", &current.read_errors);
        return false;
    }

    uint8_t header[SUMMARY_HEADER_SIZE];
    uint32_t record_count = 0;
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
    bool ok = read_all(file, header, sizeof(header)) &&
              decode_summary_header(header,
                                    record_count,
                                    payload_len,
                                    payload_crc);
    ReportSpoolBuffer payload;
    if (ok && payload_len && !payload.reserve_capacity(payload_len)) {
        ok = false;
    }
    if (ok && payload_len) {
        size_t offset = 0;
        uint8_t *dst = payload.append_uninitialized(payload_len, offset);
        ok = dst && read_all(file, dst, payload_len);
    }
    file.close();

    if (!ok) {
        note_error("summary_read_failed", &current.read_errors);
        return false;
    }
    const uint32_t actual_crc = payload.size()
        ? crc32_ieee(payload.data(), payload.size())
        : crc32_ieee(nullptr, 0);
    if (actual_crc != payload_crc) {
        note_error("summary_crc_failed", &current.read_errors);
        return false;
    }

    for (uint32_t i = 0; i < record_count; ++i) {
        ReportSummaryRecord record;
        const uint8_t *raw = payload.data() + i * SUMMARY_RECORD_SIZE;
        if (!decode_summary_record(raw, record)) {
            note_error("summary_record_invalid", &current.read_errors);
            return false;
        }
        if (!callback(context, record)) {
            note_error("summary_callback_rejected", &current.read_errors);
            return false;
        }
    }

    current.summary_records_read += record_count;
    current.bytes_read += payload.size();
    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

bool clear_summary_records(uint32_t &deleted) {
    Storage::Guard g;
    deleted = 0;
    char path[REPORT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/summary/nights.idx", BASE_DIR);
    const bool existed = Storage::exists(path);
    if (!Storage::remove(path)) {
        note_error("summary_remove_failed", &current.write_errors);
        return false;
    }
    if (existed) deleted = 1;
    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

bool reset_cache_store(uint32_t &renamed) {
    Storage::Guard g;
    renamed = 0;
    if (!ready()) {
        note_error("storage_unavailable", &current.layout_errors);
        return false;
    }

    if (Storage::exists(BASE_DIR)) {
        char trash_path[REPORT_PATH_MAX];
        const int written =
            snprintf(trash_path,
                     sizeof(trash_path),
                     "/aircannect/report/v2-trash-%lu",
                     static_cast<unsigned long>(millis()));
        if (written <= 0 ||
            static_cast<size_t>(written) >= sizeof(trash_path)) {
            note_error("bad_report_trash_path", &current.layout_errors);
            return false;
        }
        if (!Storage::rename(BASE_DIR, trash_path)) {
            note_error("report_store_rename_failed", &current.layout_errors);
            return false;
        }
        renamed = 1;
    }

    if (!ensure_layout()) return false;
    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

bool remove_tree_step(const char *path,
                      uint32_t &budget,
                      uint32_t &removed) {
    if (!path || !path[0] || budget == 0) return true;

    File node = Storage::open(path, "r");
    if (!node) {
        if (!Storage::remove(path)) return false;
        if (budget > 0) budget--;
        removed++;
        return true;
    }

    if (!node.isDirectory()) {
        node.close();
        if (!Storage::remove(path)) return false;
        if (budget > 0) budget--;
        removed++;
        return true;
    }

    while (budget > 0) {
        File child = node.openNextFile();
        if (!child) break;

        char child_path[REPORT_PATH_MAX];
        const bool path_ok =
            build_child_path(path,
                             child.name(),
                             child_path,
                             sizeof(child_path));
        const bool child_is_dir = child.isDirectory();
        child.close();
        if (!path_ok) {
            node.close();
            note_error("trash_child_path_failed", &current.write_errors);
            return false;
        }

        if (child_is_dir) {
            if (!remove_tree_step(child_path, budget, removed)) {
                node.close();
                return false;
            }
        } else {
            if (!Storage::remove(child_path)) {
                node.close();
                return false;
            }
            budget--;
            removed++;
        }
    }
    node.close();

    if (budget == 0) return true;
    if (!Storage::rmdir(path)) return false;
    budget--;
    removed++;
    return true;
}

bool cleanup_trash_step(uint32_t max_entries, uint32_t &removed) {
    Storage::Guard g;
    removed = 0;
    if (!max_entries) return true;
    if (!ready()) return false;

    File root = Storage::open("/aircannect/report", "r");
    if (!root) return true;
    if (!root.isDirectory()) {
        root.close();
        return true;
    }

    uint32_t budget = max_entries;
    bool ok = true;
    while (budget > 0) {
        File child = root.openNextFile();
        if (!child) break;
        const bool child_is_dir = child.isDirectory();
        const bool child_is_trash = is_report_trash_dir_name(child.name());
        char child_path[REPORT_PATH_MAX];
        const bool path_ok =
            build_child_path("/aircannect/report",
                             child.name(),
                             child_path,
                             sizeof(child_path));
        child.close();

        if (!child_is_dir || !child_is_trash) continue;
        if (!path_ok) {
            ok = false;
            note_error("trash_path_failed", &current.write_errors);
            break;
        }
        if (!remove_tree_step(child_path, budget, removed)) {
            ok = false;
            note_error("trash_cleanup_failed", &current.write_errors);
            break;
        }
    }
    root.close();
    if (ok) set_error(current.last_error, sizeof(current.last_error), "");
    return ok;
}

bool write_coverage_batch(const char *source,
                          const ReportStoreCoverageRecord *records,
                          size_t count) {
    if (!source || !source[0]) {
        note_error("bad_coverage_source", &current.coverage_write_errors);
        return false;
    }
    ReportStoreCoverageRecord *scratch = ensure_coverage_scratch();
    if (!scratch) {
        note_error("coverage_scratch_alloc_failed",
                   &current.coverage_write_errors);
        return false;
    }

    Storage::Guard g;
    if (!ensure_layout()) return false;

    // One load -> coalesce ALL records -> one atomic rewrite. Doing this per
    // record (the old per-night call) re-read+rewrote the whole file each time,
    // O(nights x filesize) SD on the main loop -> CAN RX starves -> framing CRC.
    size_t set_count = load_coverage(source, scratch);
    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        // Only Complete intervals are persisted; an Incomplete span is just a gap.
        if (records[i].state != ReportStoreCoverageState::Complete) continue;
        if (!valid_coverage_record(records[i])) {
            note_error("bad_coverage_record", &current.coverage_write_errors);
            continue;
        }
        coverage_insert(scratch, set_count, records[i]);
        ++added;
    }
    if (added == 0) {
        set_error(current.last_error, sizeof(current.last_error), "");
        return true;
    }
    if (!rewrite_coverage_file(source, scratch, set_count)) {
        note_error("coverage_write_failed", &current.coverage_write_errors);
        return false;
    }
    current.coverage_records_written += static_cast<uint32_t>(added);
    set_error(current.last_error, sizeof(current.last_error), "");
    Log::logf(CAT_RPC, LOG_DEBUG,
              "[REPORT] coverage source=%s intervals=%u added=%u\n",
              source, static_cast<unsigned>(set_count),
              static_cast<unsigned>(added));
    return true;
}

bool write_coverage(const char *source,
                    const ReportStoreCoverageRecord &record) {
    return write_coverage_batch(source, &record, 1);
}

bool coverage_complete(const char *source,
                       int64_t start_ms,
                       int64_t end_ms,
                       uint32_t parser_schema) {
    int64_t missing_ms = start_ms;
    if (!coverage_first_missing(source,
                                start_ms,
                                end_ms,
                                parser_schema,
                                missing_ms)) {
        return false;
    }
    return missing_ms >= end_ms;
}

bool coverage_first_missing(const char *source,
                            int64_t start_ms,
                            int64_t end_ms,
                            uint32_t parser_schema,
                            int64_t &missing_ms) {
    missing_ms = start_ms;
    if (!source || !source[0] || start_ms < 0 || end_ms <= start_ms ||
        parser_schema == 0) {
        note_error("bad_coverage_query", &current.coverage_read_errors);
        return false;
    }
    ReportStoreCoverageRecord *scratch = ensure_coverage_scratch();
    if (!scratch) {
        note_error("coverage_scratch_alloc_failed",
                   &current.coverage_read_errors);
        return false;
    }

    Storage::Guard g;
    const size_t count = load_coverage(source, scratch);
    const int64_t tol = AC_REPORT_COVERAGE_TOLERANCE_MS;
    int64_t covered_until = start_ms;
    // scratch is sorted by start and same-tag coalesced, so one walk over the
    // matching-schema intervals finds the first gap. Tolerance absorbs small
    // joint/boundary slack; a real interior gap (minutes) is not masked.
    for (size_t i = 0; i < count; ++i) {
        const ReportStoreCoverageRecord &iv = scratch[i];
        if (iv.parser_schema != parser_schema) continue;
        if (iv.start_ms > covered_until + tol) break;  // gap before this span
        if (iv.end_ms > covered_until) covered_until = iv.end_ms;
        if (covered_until + tol >= end_ms) {
            missing_ms = end_ms;
            set_error(current.last_error, sizeof(current.last_error), "");
            return true;
        }
    }
    missing_ms = covered_until < end_ms ? covered_until : end_ms;
    return true;
}

bool clear_coverage(const char *source,
                    int64_t start_ms,
                    int64_t end_ms,
                    uint32_t &deleted) {
    Storage::Guard g;
    deleted = 0;
    if (!source || !source[0] || start_ms < 0 || end_ms <= start_ms) {
        note_error("bad_coverage_clear_query",
                   &current.coverage_write_errors);
        return false;
    }

    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) {
        note_error("bad_coverage_path", &current.coverage_write_errors);
        return false;
    }
    if (!Storage::exists(path)) return true;

    char tmp_path[REPORT_PATH_MAX + 8];
    const int tmp_written =
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tmp_written <= 0 ||
        static_cast<size_t>(tmp_written) >= sizeof(tmp_path)) {
        note_error("bad_coverage_tmp", &current.coverage_write_errors);
        return false;
    }
    Storage::remove(tmp_path);

    File in = Storage::open(path, "r");
    File out = Storage::open(tmp_path, "w");
    if (!in || !out) {
        if (in) in.close();
        if (out) out.close();
        Storage::remove(tmp_path);
        note_error("coverage_clear_open_failed",
                   &current.coverage_write_errors);
        return false;
    }

    bool ok = true;
    bool kept = false;
    while (true) {
        uint8_t raw[COVERAGE_RECORD_SIZE];
        const int got = in.read(raw, sizeof(raw));
        if (got == 0) break;
        if (got != static_cast<int>(sizeof(raw))) {
            ok = false;
            note_error("coverage_short_read",
                       &current.coverage_write_errors);
            break;
        }
        ReportStoreCoverageRecord record;
        if (!decode_coverage_record(raw, record)) {
            // Drop unreadable/old-schema records during a clear (they are
            // effectively removed) instead of aborting the whole rewrite.
            deleted++;
            continue;
        }
        if (ranges_overlap(record.start_ms,
                           record.end_ms,
                           start_ms,
                           end_ms)) {
            deleted++;
            continue;
        }
        if (!write_all(out, raw, sizeof(raw))) {
            ok = false;
            note_error("coverage_rewrite_failed",
                       &current.coverage_write_errors);
            break;
        }
        kept = true;
    }
    in.close();
    out.close();

    if (!ok) {
        Storage::remove(tmp_path);
        return false;
    }
    if (!Storage::remove(path)) {
        Storage::remove(tmp_path);
        note_error("coverage_remove_failed", &current.coverage_write_errors);
        return false;
    }
    if (kept) {
        if (!Storage::rename(tmp_path, path)) {
            Storage::remove(tmp_path);
            note_error("coverage_commit_failed",
                       &current.coverage_write_errors);
            return false;
        }
    } else {
        Storage::remove(tmp_path);
    }
    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

const char *kind_name(ReportStoreChunkKind kind) {
    switch (kind) {
        case ReportStoreChunkKind::Events: return "events";
        case ReportStoreChunkKind::Series:
        default: return "series";
    }
}

}  // namespace ReportStore
}  // namespace aircannect
