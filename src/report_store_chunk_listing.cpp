#include "report_store.h"

#include <stdio.h>

#include "report_store_internal.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStore {

using namespace ReportStoreInternal;

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

}  // namespace ReportStore
}  // namespace aircannect
