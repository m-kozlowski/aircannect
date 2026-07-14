#include "report_store_internal.h"

#include <string.h>

#include "crc32.h"
#include "little_endian.h"
#include "report_summary_record_codec.h"

namespace aircannect {
namespace ReportStoreInternal {

namespace {

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

uint32_t string_hash(const char *value) {
    return value && value[0]
        ? crc32_ieee(reinterpret_cast<const uint8_t *>(value), strlen(value))
        : crc32_ieee(nullptr, 0);
}

}  // namespace

bool valid_key(const ReportStoreChunkKey &key) {
    return key.source && key.source[0] && key.name && key.name[0] &&
           key.start_ms >= 0 && key.end_ms > key.start_ms &&
           (key.origin == ReportStoreChunkOrigin::Spool ||
            key.origin == ReportStoreChunkOrigin::Live);
}

bool valid_chunk_meta(const ReportStoreChunkMeta &meta) {
    return meta.payload_schema != 0;
}

bool valid_coverage_record(const ReportStoreCoverageRecord &record) {
    return record.start_ms >= 0 && record.end_ms > record.start_ms &&
           record.parser_schema != 0 &&
           (record.state == ReportStoreCoverageState::Complete ||
            record.state == ReportStoreCoverageState::Incomplete);
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

bool append_summary_record(ReportSpoolBuffer &payload,
                           const ReportSummaryRecord &record) {
    size_t offset = 0;
    uint8_t *raw = payload.append_uninitialized(SUMMARY_RECORD_SIZE, offset);
    if (!raw) return false;
    return report_summary_record_encode(raw, SUMMARY_RECORD_SIZE, record);
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
                   uint32_t &payload_crc,
                   ReportStoreChunkOrigin *origin_out) {
    if (get_le32(header + 0) != CHUNK_MAGIC) return false;
    if (get_le16(header + 4) != CHUNK_SCHEMA) return false;
    if (get_le16(header + 6) != CHUNK_HEADER_SIZE) return false;
    if (header[8] != static_cast<uint8_t>(key.kind)) return false;

    const ReportStoreChunkOrigin origin =
        static_cast<ReportStoreChunkOrigin>(header[9]);
    if (origin != ReportStoreChunkOrigin::Spool &&
        origin != ReportStoreChunkOrigin::Live) {
        return false;
    }
    if (origin_out) {
        *origin_out = origin;
    } else if (origin != key.origin) {
        return false;
    }
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
    if (crc32_ieee(raw, CHUNK_INDEX_RECORD_SIZE - 4) != get_le32(raw + 60)) {
        return false;
    }
    if (raw[8] != static_cast<uint8_t>(query.kind)) return false;

    const ReportStoreChunkOrigin origin =
        static_cast<ReportStoreChunkOrigin>(raw[9]);
    if (origin != ReportStoreChunkOrigin::Spool &&
        origin != ReportStoreChunkOrigin::Live) {
        return false;
    }
    if (get_le32(raw + 12) != string_hash(query.source)) return false;
    if (get_le32(raw + 16) != string_hash(query.name)) return false;

    info = {};
    info.key.kind = query.kind;
    info.key.source = query.source;
    info.key.name = query.name;
    info.key.origin = origin;
    info.key.start_ms = static_cast<int64_t>(get_le64(raw + 20));
    info.key.end_ms = static_cast<int64_t>(get_le64(raw + 28));
    info.key.night_start_ms = query.night_start_ms;
    info.meta.payload_schema = get_le32(raw + 36);
    info.meta.record_count = get_le32(raw + 40);
    info.payload_len = get_le32(raw + 44);
    return valid_key(info.key) && valid_chunk_meta(info.meta);
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
        eof = true;  // truncated tail: can't resync on a fixed-size stream
        return false;
    }

    // false here (full record, bad magic/schema/CRC) is a skippable stale record
    return decode_coverage_record(raw, record);
}

}  // namespace ReportStoreInternal
}  // namespace aircannect
