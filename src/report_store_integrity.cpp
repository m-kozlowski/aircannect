#include "report_store.h"

#include <Arduino.h>
#include <stdio.h>

#include "report_store_internal.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStore {

using namespace ReportStoreInternal;

namespace {

void integrity_note_error(ReportStoreIntegrityResult &out,
                          const char *error) {
    out.errors++;
    copy_cstr(out.last_error, sizeof(out.last_error), error);
}

bool check_chunk_index_integrity(const ReportStoreChunkKey &dir_key,
                                 uint32_t valid_chunks,
                                 ReportStoreIntegrityResult &out) {
    char index_path[REPORT_PATH_MAX];
    if (!build_chunk_index_path(dir_key, index_path, sizeof(index_path))) {
        integrity_note_error(out, "bad_chunk_index_path");
        return false;
    }
    if (!Storage::exists(index_path)) {
        if (valid_chunks > 0) out.chunk_indexes_missing++;
        return true;
    }

    File index = Storage::open(index_path, "r");
    if (!index) {
        integrity_note_error(out, "chunk_index_open_failed");
        return false;
    }

    bool ok = true;
    uint8_t raw[CHUNK_INDEX_RECORD_SIZE];
    for (;;) {
        const int got = index.read(raw, sizeof(raw));
        if (got == 0) break;
        if (got != static_cast<int>(sizeof(raw))) {
            out.chunk_indexes_invalid++;
            break;
        }

        ReportStoreChunkInfo info;
        if (!decode_chunk_index_record(raw, dir_key, info) ||
            !chunk_index_record_matches_payload(info)) {
            out.chunk_indexes_invalid++;
            continue;
        }
    }

    index.close();
    return ok;
}

bool check_chunk_leaf_integrity(ReportStoreChunkKind kind,
                                const char *source,
                                const char *name,
                                int64_t night_start_ms,
                                ReportStoreIntegrityResult &out,
                                bool repair) {
    ReportStoreChunkKey dir_key;
    dir_key.kind = kind;
    dir_key.source = source;
    dir_key.name = name;
    dir_key.start_ms = 0;
    dir_key.end_ms = 1;
    dir_key.night_start_ms = night_start_ms;

    char dir_path[REPORT_PATH_MAX];
    if (!build_dir_path(dir_key, dir_path, sizeof(dir_path))) {
        integrity_note_error(out, "bad_chunk_path");
        return false;
    }

    File dir = Storage::open(dir_path, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        integrity_note_error(out, "chunk_path_not_dir");
        return false;
    }
    out.chunk_dirs_checked++;

    bool ok = true;
    uint32_t valid_chunks = 0;
    bool removed_invalid = false;

    while (true) {
        File child = dir.openNextFile();
        if (!child) break;

        const bool child_is_dir = child.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), path_basename(child.name()));
        child.close();
        if (child_is_dir) continue;

        int64_t start_ms = 0;
        int64_t end_ms = 0;
        if (!parse_chunk_filename(child_name, start_ms, end_ms)) continue;
        out.chunks_checked++;

        ReportStoreChunkKey key = dir_key;
        key.start_ms = start_ms;
        key.end_ms = end_ms;

        ReportStoreChunkInfo info;
        if (read_chunk_file_info(key, info)) {
            valid_chunks++;
            continue;
        }

        out.chunks_invalid++;
        if (!repair) continue;

        char chunk_path[REPORT_PATH_MAX];
        if (!build_chunk_path(key, chunk_path, sizeof(chunk_path)) ||
            !Storage::remove(chunk_path)) {
            ok = false;
            integrity_note_error(out, "chunk_remove_failed");
            break;
        }

        out.chunks_removed++;
        out.repaired = true;
        removed_invalid = true;
    }

    dir.close();

    const uint32_t missing_before = out.chunk_indexes_missing;
    const uint32_t invalid_before = out.chunk_indexes_invalid;
    if (!check_chunk_index_integrity(dir_key, valid_chunks, out)) ok = false;

    const bool index_changed = out.chunk_indexes_missing != missing_before ||
                               out.chunk_indexes_invalid != invalid_before;
    if (repair && ok && (valid_chunks > 0 || removed_invalid ||
                         index_changed)) {
        if (!rebuild_chunk_index_from_directory(dir_key)) {
            ok = false;
            integrity_note_error(out, "chunk_index_rebuild_failed");
        } else {
            out.chunk_indexes_rebuilt++;
            out.repaired = true;
        }
    }

    return ok;
}

bool scan_chunk_name_dir(ReportStoreChunkKind kind,
                         const char *source,
                         const char *name,
                         ReportStoreIntegrityResult &out,
                         bool repair) {
    ReportStoreChunkKey name_key;
    name_key.kind = kind;
    name_key.source = source;
    name_key.name = name;
    name_key.start_ms = 0;
    name_key.end_ms = 1;

    char name_path[REPORT_PATH_MAX];
    if (!build_dir_path(name_key, name_path, sizeof(name_path))) {
        integrity_note_error(out, "bad_chunk_path");
        return false;
    }

    File dir = Storage::open(name_path, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        integrity_note_error(out, "chunk_name_not_dir");
        return false;
    }

    bool ok = true;
    bool has_legacy_chunks = false;

    while (true) {
        File child = dir.openNextFile();
        if (!child) break;

        const bool child_is_dir = child.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), path_basename(child.name()));
        child.close();

        if (child_is_dir) {
            int64_t night_start_ms = 0;
            if (!parse_dir_int64(child_name, night_start_ms)) continue;
            if (!check_chunk_leaf_integrity(kind,
                                            source,
                                            name,
                                            night_start_ms,
                                            out,
                                            repair)) {
                ok = false;
            }
            continue;
        }

        int64_t start_ms = 0;
        int64_t end_ms = 0;
        if (parse_chunk_filename(child_name, start_ms, end_ms)) {
            has_legacy_chunks = true;
        }
    }

    dir.close();

    if (has_legacy_chunks &&
        !check_chunk_leaf_integrity(kind, source, name, 0, out, repair)) {
        ok = false;
    }

    return ok;
}

bool scan_chunk_kind_integrity(ReportStoreChunkKind kind,
                               ReportStoreIntegrityResult &out,
                               bool repair) {
    char kind_path[REPORT_PATH_MAX];
    const int written = snprintf(kind_path, sizeof(kind_path), "%s/%s",
                                 BASE_DIR, kind_name(kind));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(kind_path)) {
        integrity_note_error(out, "bad_chunk_kind_path");
        return false;
    }

    File kind_dir = Storage::open(kind_path, "r");
    if (!kind_dir) return true;
    if (!kind_dir.isDirectory()) {
        kind_dir.close();
        integrity_note_error(out, "chunk_kind_not_dir");
        return false;
    }

    bool ok = true;

    while (true) {
        File source_dir = kind_dir.openNextFile();
        if (!source_dir) break;

        const bool source_is_dir = source_dir.isDirectory();
        char source_name[72];
        copy_cstr(source_name,
                  sizeof(source_name),
                  path_basename(source_dir.name()));
        source_dir.close();
        if (!source_is_dir || !source_name[0]) continue;

        char source_path[REPORT_PATH_MAX];
        const int source_written =
            snprintf(source_path, sizeof(source_path), "%s/%s",
                     kind_path, source_name);
        if (source_written <= 0 ||
            static_cast<size_t>(source_written) >= sizeof(source_path)) {
            ok = false;
            integrity_note_error(out, "bad_chunk_source_path");
            continue;
        }

        File names = Storage::open(source_path, "r");
        if (!names) continue;
        if (!names.isDirectory()) {
            names.close();
            integrity_note_error(out, "chunk_source_not_dir");
            ok = false;
            continue;
        }

        while (true) {
            File name_dir = names.openNextFile();
            if (!name_dir) break;

            const bool name_is_dir = name_dir.isDirectory();
            char name_name[72];
            copy_cstr(name_name,
                      sizeof(name_name),
                      path_basename(name_dir.name()));
            name_dir.close();
            if (!name_is_dir || !name_name[0]) continue;

            if (!scan_chunk_name_dir(kind,
                                     source_name,
                                     name_name,
                                     out,
                                     repair)) {
                ok = false;
            }
        }

        names.close();
    }

    kind_dir.close();
    return ok;
}

}  // namespace

bool check_integrity(bool repair, ReportStoreIntegrityResult &out) {
    Storage::Guard g;
    out = {};
    current.available = Storage::mounted();
    if (!current.available) {
        integrity_note_error(out, "storage_unavailable");
        return false;
    }
    if (!ensure_layout()) {
        integrity_note_error(out, "layout_failed");
        return false;
    }

    bool ok = true;
    if (!check_summary_integrity(out, repair)) ok = false;
    if (!check_coverage_integrity(out, repair)) ok = false;
    if (!scan_chunk_kind_integrity(ReportStoreChunkKind::Series,
                                   out,
                                   repair)) {
        ok = false;
    }
    if (!scan_chunk_kind_integrity(ReportStoreChunkKind::Events,
                                   out,
                                   repair)) {
        ok = false;
    }

    const bool dirty =
        out.summary_invalid > out.summary_removed ||
        out.chunks_invalid > out.chunks_removed ||
        out.chunk_indexes_missing > out.chunk_indexes_rebuilt ||
        out.chunk_indexes_invalid > out.chunk_indexes_rebuilt ||
        out.coverage_records_dropped > 0;
    out.ok = ok && out.errors == 0 && (!dirty || repair);
    if (out.ok) {
        set_error(current.last_error, sizeof(current.last_error), "");
    } else if (out.last_error[0]) {
        set_error(current.last_error,
                  sizeof(current.last_error),
                  out.last_error);
    }

    return out.ok;
}

}  // namespace ReportStore
}  // namespace aircannect
