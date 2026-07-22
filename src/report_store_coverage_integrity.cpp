#include "report_store_internal.h"

#include <stdio.h>
#include <string.h>

#include "report_legacy_storage.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStoreInternal {

namespace {

void integrity_note_error(ReportStoreIntegrityResult &out,
                          const char *error) {
    out.errors++;
    copy_cstr(out.last_error, sizeof(out.last_error), error);
}

bool check_coverage_file_integrity(const char *source,
                                   ReportStoreIntegrityResult &out,
                                   bool repair) {
    if (!source || !source[0]) return true;

    ReportStoreCoverageRecord *scratch = ensure_coverage_scratch();
    if (!scratch) {
        integrity_note_error(out, "coverage_scratch_alloc_failed");
        return false;
    }

    size_t count = 0;
    uint32_t dropped = 0;

    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) {
        integrity_note_error(out, "bad_coverage_path");
        return false;
    }

    ReportLegacyFile file = ReportLegacyStorage::open(path, "r");
    if (!file) {
        integrity_note_error(out, "coverage_open_failed");
        return false;
    }
    out.coverage_files_checked++;

    for (;;) {
        bool eof = false;
        ReportStoreCoverageRecord record;
        if (!read_coverage_record(file, record, eof)) {
            if (eof) break;
            dropped++;
            continue;
        }
        out.coverage_records_checked++;

        const size_t before = count;
        coverage_insert(scratch, count, record);
        if (count == before) {
            dropped++;
        }
    }
    file.close();

    out.coverage_records_dropped += dropped;
    if (!repair || dropped == 0) return true;
    if (!rewrite_coverage_file(source, scratch, count)) {
        integrity_note_error(out, "coverage_rewrite_failed");
        return false;
    }

    out.coverage_files_rewritten++;
    out.repaired = true;
    return true;
}

}  // namespace

bool check_coverage_integrity(ReportStoreIntegrityResult &out, bool repair) {
    char root_path[REPORT_PATH_MAX];
    snprintf(root_path, sizeof(root_path), "%s/coverage", BASE_DIR);

    ReportLegacyFile root = ReportLegacyStorage::open(root_path, "r");
    if (!root) return true;
    if (!root.isDirectory()) {
        root.close();
        integrity_note_error(out, "coverage_path_not_dir");
        return false;
    }

    bool ok = true;
    while (true) {
        ReportLegacyFile child = root.openNextFile();
        if (!child) break;

        const bool child_is_dir = child.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), path_basename(child.name()));
        child.close();
        if (child_is_dir || !name_ends_with(child_name, ".idx")) continue;

        char source[72];
        copy_cstr(source, sizeof(source), child_name);
        char *suffix = strstr(source, ".idx");
        if (suffix) *suffix = '\0';
        if (!check_coverage_file_integrity(source, out, repair)) ok = false;
    }
    root.close();
    return ok;
}

}  // namespace ReportStoreInternal
}  // namespace aircannect
