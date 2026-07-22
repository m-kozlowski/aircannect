#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

#include "report_legacy_storage.h"
#include "report_store.h"
#include "report_summary_record_codec.h"

namespace aircannect {
namespace ReportStoreInternal {

static constexpr const char *BASE_DIR = "/aircannect/report/v3";
static constexpr uint32_t CHUNK_MAGIC = 0x50524341u;  // "ACRP", little-endian.
static constexpr uint16_t CHUNK_SCHEMA = 3;  // bumped for chunk origin byte
static constexpr size_t CHUNK_HEADER_SIZE = 56;
static constexpr uint32_t SUMMARY_MAGIC = 0x53524341u;  // "ACRS", little-endian.
static constexpr uint16_t SUMMARY_SCHEMA = 5;
static constexpr size_t SUMMARY_HEADER_SIZE = 32;
static constexpr size_t SUMMARY_RECORD_SIZE = AC_REPORT_SUMMARY_RECORD_CODEC_SIZE;
static constexpr uint32_t COVERAGE_MAGIC = 0x56524341u;  // "ACRV", little-endian.
static constexpr uint16_t COVERAGE_SCHEMA = 3;  // bumped: night-partitioned chunk layout
static constexpr size_t COVERAGE_RECORD_SIZE = 48;
static constexpr size_t COVERAGE_MAX_INTERVALS = 128;
static constexpr uint32_t CHUNK_INDEX_MAGIC = 0x49524341u;  // "ACRI", little-endian.
static constexpr uint16_t CHUNK_INDEX_SCHEMA = 2;  // bumped for chunk origin byte
static constexpr size_t CHUNK_INDEX_RECORD_SIZE = 64;
static constexpr size_t REPORT_PATH_MAX = 192;
static constexpr const char *REPORT_TRASH_PREFIX = "v3-trash-";
static constexpr size_t REPORT_TRASH_CLEANUP_MAX_DEPTH = 16;

extern ReportStoreStatus current;

void set_error(char *dst, size_t size, const char *error);
void note_error(const char *error, uint32_t *counter);

bool valid_key(const ReportStoreChunkKey &key);
bool valid_chunk_meta(const ReportStoreChunkMeta &meta);
bool valid_coverage_record(const ReportStoreCoverageRecord &record);
bool ranges_overlap(int64_t left_start,
                    int64_t left_end,
                    int64_t right_start,
                    int64_t right_end);
ReportStoreCoverageRecord *ensure_coverage_scratch();
void coverage_insert(ReportStoreCoverageRecord *recs,
                     size_t &count,
                     const ReportStoreCoverageRecord &rec);
size_t load_coverage(const char *source, ReportStoreCoverageRecord *recs);
bool rewrite_coverage_file(const char *source,
                           const ReportStoreCoverageRecord *recs,
                           size_t count);
bool write_all(ReportLegacyFile &file, const uint8_t *data, size_t len);
bool read_all(ReportLegacyFile &file, uint8_t *data, size_t len);

bool parse_chunk_filename(const char *name,
                          int64_t &start_ms,
                          int64_t &end_ms);
bool read_chunk_file_info(const ReportStoreChunkKey &key,
                          ReportStoreChunkInfo &info);
bool chunk_index_record_matches_payload(const ReportStoreChunkInfo &info);
bool rebuild_chunk_index_from_directory(const ReportStoreChunkKey &dir_key);
bool ensure_chunk_index_record(const ReportStoreChunkKey &key,
                               const ReportStoreChunkMeta &meta,
                               uint32_t payload_len);

void encode_summary_header(uint8_t *header,
                           uint32_t record_count,
                           uint32_t payload_len,
                           uint32_t payload_crc);
bool decode_summary_header(const uint8_t *header,
                           uint32_t &record_count,
                           uint32_t &payload_len,
                           uint32_t &payload_crc);
bool append_summary_record(ReportSpoolBuffer &payload,
                           const ReportSummaryRecord &record);

void encode_header(uint8_t *header,
                   const ReportStoreChunkKey &key,
                   const ReportStoreChunkMeta &meta,
                   size_t payload_len,
                   uint32_t payload_crc);
bool decode_header(const uint8_t *header,
                   const ReportStoreChunkKey &key,
                   ReportStoreChunkMeta &meta,
                   uint32_t &payload_len,
                   uint32_t &payload_crc,
                   ReportStoreChunkOrigin *origin_out = nullptr);

void encode_chunk_index_record(uint8_t *raw,
                               const ReportStoreChunkKey &key,
                               const ReportStoreChunkMeta &meta,
                               uint32_t payload_len);
bool decode_chunk_index_record(const uint8_t *raw,
                               const ReportStoreChunkKey &query,
                               ReportStoreChunkInfo &info);

void encode_coverage_record(uint8_t *raw,
                            const ReportStoreCoverageRecord &record);
bool decode_coverage_record(const uint8_t *raw,
                            ReportStoreCoverageRecord &record);
bool read_coverage_record(ReportLegacyFile &file,
                          ReportStoreCoverageRecord &record,
                          bool &eof);

void sanitize_name(const char *name, char *out, size_t out_size);

bool build_dir_path(const ReportStoreChunkKey &key, char *path, size_t path_size);
bool build_source_dir_path(const ReportStoreChunkKey &key, char *path, size_t path_size);
bool build_chunk_path(const ReportStoreChunkKey &key, char *path, size_t path_size);
bool build_chunk_index_path(const ReportStoreChunkKey &key, char *path, size_t path_size);
bool build_coverage_path(const char *source, char *path, size_t path_size);
bool ensure_chunk_dir(const ReportStoreChunkKey &key);

const char *path_basename(const char *path);
bool build_child_path(const char *parent,
                      const char *child_name,
                      char *out,
                      size_t out_size);
bool name_ends_with(const char *name, const char *suffix);
bool parse_dir_int64(const char *name, int64_t &out);
bool is_report_trash_dir_name(const char *name);

bool check_summary_integrity(ReportStoreIntegrityResult &out, bool repair);
bool check_coverage_integrity(ReportStoreIntegrityResult &out, bool repair);

}  // namespace ReportStoreInternal
}  // namespace aircannect
