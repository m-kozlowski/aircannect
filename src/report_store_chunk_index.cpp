#include "report_store.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "report_store_internal.h"
#include "report_legacy_storage.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStoreInternal {
namespace {

bool chunk_index_record_matches(const ReportStoreChunkInfo &info,
                                const ReportStoreChunkKey &key,
                                const ReportStoreChunkMeta &meta,
                                uint32_t payload_len) {
    return info.key.origin == key.origin && info.key.start_ms == key.start_ms &&
           info.key.end_ms == key.end_ms &&
           info.key.night_start_ms == key.night_start_ms &&
           info.meta.payload_schema == meta.payload_schema &&
           info.meta.record_count == meta.record_count &&
           info.payload_len == payload_len;
}

bool chunk_index_record_same_file(const ReportStoreChunkInfo &info,
                                  const ReportStoreChunkKey &key) {
    return info.key.start_ms == key.start_ms &&
           info.key.end_ms == key.end_ms &&
           info.key.night_start_ms == key.night_start_ms;
}

bool rewrite_chunk_index_with_record(const ReportStoreChunkKey &key,
                                     const ReportStoreChunkMeta &meta,
                                     uint32_t payload_len) {
    char path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(key, path, sizeof(path))) {
        note_error("bad_chunk_index_path", &current.write_errors);
        return false;
    }

    char tmp_path[REPORT_PATH_MAX + 8];
    const int tmp_written =
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tmp_written <= 0 ||
        static_cast<size_t>(tmp_written) >= sizeof(tmp_path)) {
        note_error("bad_chunk_index_tmp", &current.write_errors);
        return false;
    }

    ReportLegacyStorage::remove(tmp_path);

    ReportLegacyFile tmp = ReportLegacyStorage::open(tmp_path, "w");
    if (!tmp) {
        note_error("chunk_index_tmp_open_failed", &current.write_errors);
        return false;
    }

    bool ok = true;
    uint8_t raw[CHUNK_INDEX_RECORD_SIZE];

    if (ReportLegacyStorage::exists(path)) {
        ReportLegacyFile index = ReportLegacyStorage::open(path, "r");
        if (!index) {
            tmp.close();
            ReportLegacyStorage::remove(tmp_path);
            note_error("chunk_index_open_failed", &current.write_errors);
            return false;
        }

        for (;;) {
            const int got = index.read(raw, sizeof(raw));
            if (got == 0) break;
            if (got != static_cast<int>(sizeof(raw))) break;

            ReportStoreChunkInfo info;
            if (!decode_chunk_index_record(raw, key, info)) continue;
            if (chunk_index_record_same_file(info, key)) continue;
            if (!chunk_index_record_matches_payload(info)) continue;

            if (!write_all(tmp, raw, sizeof(raw))) {
                ok = false;
                note_error("chunk_index_rewrite_failed",
                           &current.write_errors);
                break;
            }
        }

        index.close();
    }

    if (ok) {
        encode_chunk_index_record(raw, key, meta, payload_len);
        ok = write_all(tmp, raw, sizeof(raw));
        if (!ok) {
            note_error("chunk_index_write_failed", &current.write_errors);
        }
    }

    tmp.close();

    if (!ok) {
        ReportLegacyStorage::remove(tmp_path);
        return false;
    }

    ReportLegacyStorage::remove(path);

    if (!ReportLegacyStorage::rename(tmp_path, path)) {
        ReportLegacyStorage::remove(tmp_path);
        note_error("chunk_index_commit_failed", &current.write_errors);
        return false;
    }

    return true;
}

bool chunk_index_contains(const ReportStoreChunkKey &key,
                          const ReportStoreChunkMeta &meta,
                          uint32_t payload_len) {
    char path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(key, path, sizeof(path)) ||
        !ReportLegacyStorage::exists(path)) {
        return false;
    }

    ReportLegacyFile file = ReportLegacyStorage::open(path, "r");
    if (!file) return false;

    bool found = false;
    uint8_t raw[CHUNK_INDEX_RECORD_SIZE];

    while (file.read(raw, sizeof(raw)) == static_cast<int>(sizeof(raw))) {
        ReportStoreChunkInfo info;
        if (!decode_chunk_index_record(raw, key, info)) continue;
        if (chunk_index_record_matches(info, key, meta, payload_len) &&
            chunk_index_record_matches_payload(info)) {
            found = true;
            break;
        }
    }

    file.close();
    return found;
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

}  // namespace

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

bool read_chunk_file_info(const ReportStoreChunkKey &key,
                          ReportStoreChunkInfo &info) {
    char path[REPORT_PATH_MAX];
    if (!build_chunk_path(key, path, sizeof(path))) return false;

    ReportLegacyFile file = ReportLegacyStorage::open(path, "r");
    if (!file) return false;

    uint8_t header[CHUNK_HEADER_SIZE];
    uint32_t payload_crc = 0;
    ReportStoreChunkOrigin origin = ReportStoreChunkOrigin::Spool;

    bool ok = read_all(file, header, sizeof(header)) &&
              decode_header(header,
                            key,
                            info.meta,
                            info.payload_len,
                            payload_crc,
                            &origin);
    if (ok) {
        const size_t expected_size =
            CHUNK_HEADER_SIZE + static_cast<size_t>(info.payload_len);
        ok = file.size() == expected_size;
    }

    file.close();

    if (!ok) {
        info = {};
        return false;
    }

    info.key = key;
    info.key.origin = origin;
    return true;
}

bool chunk_index_record_matches_payload(const ReportStoreChunkInfo &info) {
    ReportStoreChunkInfo actual;
    if (!read_chunk_file_info(info.key, actual)) return false;

    return chunk_index_record_matches(actual,
                                      info.key,
                                      info.meta,
                                      info.payload_len);
}

bool rebuild_chunk_index_from_directory(const ReportStoreChunkKey &dir_key) {
    char dir_path[REPORT_PATH_MAX];
    char index_path[REPORT_PATH_MAX];
    if (!build_dir_path(dir_key, dir_path, sizeof(dir_path)) ||
        !build_chunk_index_path(dir_key, index_path, sizeof(index_path))) {
        note_error("bad_chunk_index_path", &current.write_errors);
        return false;
    }

    char tmp_path[REPORT_PATH_MAX + 8];
    const int tmp_written =
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", index_path);
    if (tmp_written <= 0 ||
        static_cast<size_t>(tmp_written) >= sizeof(tmp_path)) {
        note_error("bad_chunk_index_tmp", &current.write_errors);
        return false;
    }

    ReportLegacyFile dir = ReportLegacyStorage::open(dir_path, "r");
    if (!dir) {
        ReportLegacyStorage::remove(index_path);
        return true;
    }

    if (!dir.isDirectory()) {
        dir.close();
        note_error("chunk_path_not_dir", &current.write_errors);
        return false;
    }

    ReportLegacyStorage::remove(tmp_path);

    ReportLegacyFile tmp = ReportLegacyStorage::open(tmp_path, "w");
    if (!tmp) {
        dir.close();
        note_error("chunk_index_tmp_open_failed", &current.write_errors);
        return false;
    }

    bool ok = true;
    bool wrote = false;

    while (ok) {
        ReportLegacyFile child = dir.openNextFile();
        if (!child) break;

        const bool child_is_dir = child.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), child.name());
        child.close();
        if (child_is_dir) continue;

        int64_t chunk_start = 0;
        int64_t chunk_end = 0;
        if (!parse_chunk_filename(child_name, chunk_start, chunk_end)) {
            continue;
        }

        ReportStoreChunkKey key = dir_key;
        key.start_ms = chunk_start;
        key.end_ms = chunk_end;

        ReportStoreChunkInfo info;
        if (!read_chunk_file_info(key, info)) continue;

        uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
        encode_chunk_index_record(raw, info.key, info.meta, info.payload_len);

        if (!write_all(tmp, raw, sizeof(raw))) {
            ok = false;
            note_error("chunk_index_rebuild_failed", &current.write_errors);
            break;
        }

        wrote = true;
    }

    dir.close();
    tmp.close();

    if (!ok) {
        ReportLegacyStorage::remove(tmp_path);
        return false;
    }

    ReportLegacyStorage::remove(index_path);

    if (!wrote) {
        ReportLegacyStorage::remove(tmp_path);
        return true;
    }

    if (!ReportLegacyStorage::rename(tmp_path, index_path)) {
        ReportLegacyStorage::remove(tmp_path);
        note_error("chunk_index_commit_failed", &current.write_errors);
        return false;
    }

    return true;
}

bool ensure_chunk_index_record(const ReportStoreChunkKey &key,
                               const ReportStoreChunkMeta &meta,
                               uint32_t payload_len) {
    if (chunk_index_contains(key, meta, payload_len)) return true;

    return rewrite_chunk_index_with_record(key, meta, payload_len);
}

}  // namespace ReportStoreInternal
}  // namespace aircannect
