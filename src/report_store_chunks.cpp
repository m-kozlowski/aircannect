#include "report_store.h"

#include <Arduino.h>
#include <stdio.h>

#include "crc32.h"
#include "debug_log.h"
#include "report_store_internal.h"
#include "storage_manager.h"

namespace aircannect {
namespace ReportStore {

using namespace ReportStoreInternal;

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
        ReportStoreChunkInfo existing;
        if (!read_chunk_file_info(key, existing)) {
            note_error("chunk_header_failed", &current.write_errors);
            return false;
        }
        return ensure_chunk_index_record(
            existing.key, existing.meta, existing.payload_len);
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
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "stored chunk kind=%s source=%s name=%s "
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

const char *kind_name(ReportStoreChunkKind kind) {
    switch (kind) {
        case ReportStoreChunkKind::Events: return "events";
        case ReportStoreChunkKind::Series:
        default: return "series";
    }
}

}  // namespace ReportStore
}  // namespace aircannect
