#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_proto.h"
#include "report_spool_types.h"

namespace aircannect {

enum class ReportStoreChunkKind : uint8_t {
    Series = 1,
    Events = 2,
};

enum class ReportStoreCoverageState : uint8_t {
    Complete = 1,
    Incomplete = 2,
};

enum class ReportStoreChunkOrigin : uint8_t {
    Spool = 0,
    Live = 1,
};

struct ReportStoreChunkKey {
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    const char *source = nullptr;
    const char *name = nullptr;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    ReportStoreChunkOrigin origin = ReportStoreChunkOrigin::Spool;
    // Partition key: chunks live under <source>/<name>/<night_start_ms>/ so one
    // night reads from a small directory. 0 = legacy flat path.
    int64_t night_start_ms = 0;
};

struct ReportStoreChunkMeta {
    uint32_t payload_schema = 0;
    uint32_t record_count = 0;
};

struct ReportStoreChunkInfo {
    ReportStoreChunkKey key;
    ReportStoreChunkMeta meta;
    uint32_t payload_len = 0;
};

using ReportStoreChunkCallback =
    bool (*)(void *context, const ReportStoreChunkInfo &chunk);

struct ReportStoreCoverageRecord {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    uint32_t parser_schema = 0;
    uint32_t source_hash = 0;
    ReportStoreCoverageState state = ReportStoreCoverageState::Complete;
    uint16_t error_code = 0;
    ReportStoreChunkOrigin origin = ReportStoreChunkOrigin::Spool;
};

struct ReportStoreStatus {
    bool initialized = false;
    bool available = false;
    uint32_t layout_errors = 0;
    uint32_t write_errors = 0;
    uint32_t read_errors = 0;
    uint32_t coverage_write_errors = 0;
    uint32_t coverage_read_errors = 0;
    uint32_t chunks_written = 0;
    uint32_t chunks_read = 0;
    uint32_t chunks_listed = 0;
    uint32_t summary_records_written = 0;
    uint32_t summary_records_read = 0;
    uint32_t coverage_records_written = 0;
    uint32_t coverage_records_read = 0;
    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    char last_error[96] = {};
};

namespace ReportStore {

void begin();
ReportStoreStatus status();

bool ready();
bool ensure_layout();

bool chunk_exists(const ReportStoreChunkKey &key);
bool write_chunk(const ReportStoreChunkKey &key,
                 const ReportStoreChunkMeta &meta,
                 const uint8_t *payload,
                 size_t len);
bool read_chunk(const ReportStoreChunkKey &key,
                ReportStoreChunkMeta &meta,
                ReportSpoolBuffer &payload);
bool for_each_chunk(ReportStoreChunkKind kind,
                    const char *source,
                    const char *name,
                    int64_t night_start_ms,
                    int64_t start_ms,
                    int64_t end_ms,
                    ReportStoreChunkCallback callback,
                    void *context);
bool clear_chunks(ReportStoreChunkKind kind,
                  const char *source,
                  const char *name,
                  int64_t night_start_ms,
                  int64_t start_ms,
                  int64_t end_ms,
                  uint32_t &deleted);

bool write_coverage(const char *source,
                    const ReportStoreCoverageRecord &record);
bool coverage_complete(const char *source,
                       int64_t start_ms,
                       int64_t end_ms,
                       uint32_t parser_schema);
bool coverage_first_missing(const char *source,
                            int64_t start_ms,
                            int64_t end_ms,
                            uint32_t parser_schema,
                            int64_t &missing_ms);
bool clear_coverage(const char *source,
                    int64_t start_ms,
                    int64_t end_ms,
                    uint32_t &deleted);

bool write_summary_records(const ReportSummaryRecord *records,
                           size_t count);
bool read_summary_records(ReportSummaryRecordCallback callback,
                          void *context);
bool clear_summary_records(uint32_t &deleted);

bool reset_cache_store(uint32_t &renamed);
bool cleanup_trash_step(uint32_t max_entries, uint32_t &removed);

const char *kind_name(ReportStoreChunkKind kind);

}  // namespace ReportStore
}  // namespace aircannect
