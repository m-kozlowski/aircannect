#include "report_store.h"

#include <Arduino.h>
#include <stdio.h>

#include "crc32.h"
#include "debug_log.h"
#include "report_store_internal.h"
#include "storage_manager.h"
#include "string_util.h"

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
        bool dirty = false;
        while (true) {
            uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
            const int got = index.read(raw, sizeof(raw));
            if (got == 0) break;
            if (got != static_cast<int>(sizeof(raw))) {
                dirty = true;
                note_error("chunk_index_short_read", &current.read_errors);
                break;
            }
            ReportStoreChunkInfo info;
            if (!decode_chunk_index_record(raw, dir_key, info)) {
                dirty = true;
                current.read_errors++;
                continue;
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
        if (ok && dirty && !rebuild_chunk_index_from_directory(dir_key)) {
            ok = false;
        }
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
        const bool child_is_dir = file.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), file.name());
        file.close();
        if (child_is_dir) continue;

        int64_t chunk_start = 0;
        int64_t chunk_end = 0;
        if (!parse_chunk_filename(child_name, chunk_start, chunk_end) ||
            !ranges_overlap(chunk_start, chunk_end, start_ms, end_ms)) {
            continue;
        }

        ReportStoreChunkKey key;
        key.kind = kind;
        key.source = source;
        key.name = name;
        key.start_ms = chunk_start;
        key.end_ms = chunk_end;
        key.night_start_ms = night_start_ms;

        ReportStoreChunkInfo info;
        if (!read_chunk_file_info(key, info)) {
            ok = false;
            note_error("chunk_header_failed", &current.read_errors);
            break;
        }
        current.chunks_listed++;
        if (!callback(context, info)) {
            ok = false;
            note_error("chunk_callback_rejected", &current.read_errors);
            break;
        }
    }
    dir.close();
    if (ok && !rebuild_chunk_index_from_directory(dir_key)) {
        ok = false;
    }
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
                continue;
            }
            if (!chunk_index_record_matches_payload(info)) continue;
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
                if (existed && !Storage::remove(chunk_path)) {
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
        const bool child_is_dir = file.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), file.name());
        file.close();
        if (child_is_dir) continue;

        int64_t chunk_start = 0;
        int64_t chunk_end = 0;
        if (!parse_chunk_filename(child_name, chunk_start, chunk_end) ||
            !ranges_overlap(chunk_start, chunk_end, start_ms, end_ms)) {
            continue;
        }
        ReportStoreChunkKey key;
        key.kind = kind;
        key.source = source;
        key.name = name;
        key.start_ms = chunk_start;
        key.end_ms = chunk_end;
        key.night_start_ms = night_start_ms;
        char chunk_path[REPORT_PATH_MAX];
        const bool path_ok =
            build_chunk_path(key, chunk_path, sizeof(chunk_path));
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

const char *kind_name(ReportStoreChunkKind kind) {
    switch (kind) {
        case ReportStoreChunkKind::Events: return "events";
        case ReportStoreChunkKind::Series:
        default: return "series";
    }
}

}  // namespace ReportStore
}  // namespace aircannect
