#include "edf_str_storage_writer.h"

#include <FS.h>
#include <stdio.h>
#include <string.h>

#include "edf_file_reader.h"
#include "edf_file_resume.h"
#include "edf_str_file_layout.h"
#include "edf_str_record_merge.h"
#include "edf_str_timeline.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "storage_path.h"

namespace aircannect {
namespace {

static constexpr size_t WRITE_CHUNK_BYTES = 4096;
static constexpr const char *TEMP_SUFFIX = ".str-new";
static constexpr const char *BACKUP_SUFFIX = ".str-old";

bool fail(EdfStrStorageWriteResult &result,
          EdfStrStorageErrorKind kind,
          const char *error) {
    result.error_kind = kind;
    result.error = error;
    return false;
}

bool companion_path(const char *path,
                    const char *suffix,
                    char (&out)[AC_STORAGE_PATH_MAX]) {
    if (!path || !suffix) return false;
    const int written = snprintf(out, sizeof(out), "%s%s", path, suffix);
    return written > 0 && static_cast<size_t>(written) < sizeof(out);
}

bool write_exact(File &file, const uint8_t *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = length - offset;
        const size_t chunk = remaining < WRITE_CHUNK_BYTES
                                 ? remaining
                                 : WRITE_CHUNK_BYTES;
        const size_t written = file.write(bytes + offset, chunk);
        if (written != chunk) return false;
        offset += written;
    }
    return true;
}

bool recover_replacement(const char *path,
                         const char *temporary,
                         const char *backup,
                         bool &recovered) {
    recovered = Storage::exists(temporary) || Storage::exists(backup);
    if (Storage::exists(path)) {
        return Storage::remove(temporary) && Storage::remove(backup);
    }

    if (Storage::exists(backup) && !Storage::rename(backup, path)) {
        return false;
    }
    return Storage::remove(temporary) && Storage::remove(backup);
}

bool publish_replacement(const char *path,
                         const uint8_t *header,
                         size_t header_size,
                         const uint8_t *records,
                         size_t records_size,
                         EdfStrStorageWriteResult &result) {
    char temporary[AC_STORAGE_PATH_MAX] = {};
    char backup[AC_STORAGE_PATH_MAX] = {};
    if (!companion_path(path, TEMP_SUFFIX, temporary) ||
        !companion_path(path, BACKUP_SUFFIX, backup)) {
        return fail(result,
                    EdfStrStorageErrorKind::Publish,
                    "str_rewrite_path_too_long");
    }
    bool recovered = false;
    if (!recover_replacement(path, temporary, backup, recovered)) {
        return fail(result,
                    EdfStrStorageErrorKind::Publish,
                    "str_rewrite_recovery_failed");
    }

    File output = Storage::open(temporary, "w");
    if (!output) {
        return fail(result,
                    EdfStrStorageErrorKind::Open,
                    "str_rewrite_open_failed");
    }
    const bool wrote = write_exact(output, header, header_size) &&
                       write_exact(output, records, records_size);
    output.flush();
    output.close();
    if (!wrote) {
        (void)Storage::remove(temporary);
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_rewrite_write_failed");
    }

    const bool had_previous = Storage::exists(path);
    if (had_previous && !Storage::rename(path, backup)) {
        (void)Storage::remove(temporary);
        return fail(result,
                    EdfStrStorageErrorKind::Publish,
                    "str_rewrite_backup_failed");
    }
    if (!Storage::rename(temporary, path)) {
        if (had_previous) (void)Storage::rename(backup, path);
        (void)Storage::remove(temporary);
        return fail(result,
                    EdfStrStorageErrorKind::Publish,
                    "str_rewrite_publish_failed");
    }
    if (had_previous && !Storage::remove(backup)) {
        return fail(result,
                    EdfStrStorageErrorKind::Publish,
                    "str_rewrite_cleanup_failed");
    }

    result.bytes_written += header_size + records_size;
    return true;
}

bool render_header(const EdfStrStorageWriteRequest &request,
                   uint32_t record_count,
                   uint8_t *header,
                   size_t header_size) {
    EdfHeaderInfo info = request.header;
    info.record_count = record_count;
    size_t written = 0;
    return edf_render_str_header(info,
                                 header,
                                 header_size,
                                 written) &&
           written == header_size;
}

bool read_exact(File &file,
                size_t offset,
                uint8_t *bytes,
                size_t length) {
    return file.seek(offset) && file.read(bytes, length) == length;
}

bool infer_header_start_day(File &file,
                            uint32_t record_count,
                            uint8_t *scratch,
                            int16_t incoming_day,
                            int32_t &header_start_day) {
    const size_t record_size = edf_str_record_size();
    for (uint32_t i = 0; i < record_count; ++i) {
        if (!read_exact(file,
                        edf_str_record_offset(i),
                        scratch,
                        record_size)) {
            return false;
        }

        const int16_t date =
            edf_str_record_date_sample(scratch, record_size);
        if (!edf_str_date_sample_valid(date) || date < i) continue;
        header_start_day = static_cast<int32_t>(date) -
                           static_cast<int32_t>(i);
        return true;
    }

    header_start_day = incoming_day;
    return true;
}

bool scan_timeline(File &file,
                   uint32_t record_count,
                   int32_t header_start_day,
                   uint8_t *scratch,
                   EdfStrTimelineScan &scan) {
    if (!edf_str_timeline_begin(header_start_day, record_count, scan)) {
        return false;
    }

    const size_t record_size = edf_str_record_size();
    for (uint32_t i = 0; i < record_count; ++i) {
        if (!read_exact(file,
                        edf_str_record_offset(i),
                        scratch,
                        record_size) ||
            !edf_str_timeline_scan_record(
                scan,
                i,
                edf_str_record_date_sample(scratch, record_size))) {
            return false;
        }
    }
    return true;
}

bool patch_record_count(File &file, uint32_t record_count) {
    char field[AC_EDF_HEADER_RECORD_COUNT_WIDTH] = {};
    if (!edf_str_format_record_count_field(record_count,
                                           field,
                                           sizeof(field)) ||
        !file.seek(AC_EDF_HEADER_RECORD_COUNT_OFFSET)) {
        return false;
    }
    return file.write(reinterpret_cast<const uint8_t *>(field),
                      sizeof(field)) == sizeof(field);
}

bool replace_existing_record(File &file,
                             uint32_t index,
                             const EdfStrStorageWriteRequest &request,
                             uint8_t *existing,
                             uint8_t *incoming,
                             EdfStrStorageWriteResult &result) {
    const size_t offset = edf_str_record_offset(index);
    if (!read_exact(file, offset, existing, request.record_size)) {
        return fail(result,
                    EdfStrStorageErrorKind::Read,
                    "str_existing_read_failed");
    }

    memcpy(incoming, request.record, request.record_size);
    const EdfStrRecordMergeStatus merge_status =
        edf_str_merge_existing_record(existing,
                                      request.record_size,
                                      incoming,
                                      request.record_size);
    if (merge_status != EdfStrRecordMergeStatus::Ok) {
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_existing_merge_failed");
    }
    if (!file.seek(offset) ||
        file.write(incoming, request.record_size) != request.record_size) {
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_existing_write_failed");
    }

    result.bytes_written += request.record_size;
    return true;
}

bool append_record(File &file,
                   uint32_t record_count,
                   const EdfStrStorageWriteRequest &request,
                   EdfStrStorageWriteResult &result) {
    if (!file.seek(edf_str_record_offset(record_count)) ||
        file.write(request.record, request.record_size) !=
            request.record_size ||
        !patch_record_count(file, record_count + 1)) {
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_append_failed");
    }

    result.bytes_written += request.record_size;
    result.record_count = record_count + 1;
    return true;
}

bool rebuild_timeline(File &file,
                      const EdfStrStorageWriteRequest &request,
                      uint32_t existing_count,
                      int32_t header_start_day,
                      const EdfStrTimelinePlan &plan,
                      uint8_t *header,
                      uint8_t *scratch,
                      EdfStrStorageWriteResult &result) {
    const size_t record_size = edf_str_record_size();
    const size_t records_size =
        static_cast<size_t>(plan.record_count) * record_size;
    uint8_t *records = static_cast<uint8_t *>(
        Memory::alloc_large(records_size, false));
    uint8_t *present = static_cast<uint8_t *>(
        Memory::calloc_large(plan.record_count, 1, false));
    if (!records || !present) {
        Memory::free(records);
        Memory::free(present);
        return fail(result,
                    EdfStrStorageErrorKind::Allocation,
                    "str_rewrite_alloc_failed");
    }

    EdfStrTimelineBuffer buffer;
    buffer.records = records;
    buffer.present = present;
    buffer.capacity = plan.record_count;
    EdfStrTimelineBuildStats stats;

    bool ok = true;
    for (uint32_t i = 0; ok && i < existing_count; ++i) {
        int32_t day = -1;
        ok = read_exact(file,
                        edf_str_record_offset(i),
                        scratch,
                        record_size) &&
             edf_str_timeline_record_day(
                 header_start_day,
                 i,
                 edf_str_record_date_sample(scratch, record_size),
                 day) &&
             edf_str_timeline_place_record(plan,
                                           buffer,
                                           day,
                                           scratch,
                                           record_size,
                                           stats);
    }

    if (ok) {
        memcpy(scratch, request.record, record_size);
        ok = edf_str_timeline_place_record(plan,
                                           buffer,
                                           edf_str_record_date_sample(
                                               request.record,
                                               record_size),
                                           scratch,
                                           record_size,
                                           stats) &&
             edf_str_timeline_fill_missing(plan, buffer, stats) &&
             edf_str_patch_header_timeline(header,
                                           edf_str_header_size(),
                                           plan.start_day,
                                           plan.record_count);
    }

    file.close();
    if (ok) {
        ok = publish_replacement(request.path,
                                 header,
                                 edf_str_header_size(),
                                 records,
                                 records_size,
                                 result);
    }

    Memory::free(records);
    Memory::free(present);
    if (!ok) {
        if (!result.error) {
            return fail(result,
                        EdfStrStorageErrorKind::Write,
                        "str_rewrite_build_failed");
        }
        return false;
    }

    result.timeline_rewritten = true;
    result.retention_applied = plan.retention_applied;
    result.record_count = plan.record_count;
    result.filler_records = stats.filler_records;
    result.merged_records = stats.merged_records;
    result.discarded_records = stats.discarded_records;
    return true;
}

bool write_fresh_file(const EdfStrStorageWriteRequest &request,
                      uint8_t *header,
                      EdfStrStorageWriteResult &result) {
    if (!render_header(request, 1, header, edf_str_header_size())) {
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_header_render_failed");
    }
    if (!publish_replacement(request.path,
                             header,
                             edf_str_header_size(),
                             request.record,
                             request.record_size,
                             result)) {
        return false;
    }

    result.record_count = 1;
    return true;
}

}  // namespace

bool edf_str_storage_write(const EdfStrStorageWriteRequest &request,
                           EdfStrStorageWriteResult &result) {
    result = {};
    if (!request.path || !request.path[0] || !request.record ||
        request.record_size != edf_str_record_size()) {
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_write_invalid");
    }
    const int16_t incoming_day =
        edf_str_record_date_sample(request.record, request.record_size);
    if (!edf_str_date_sample_valid(incoming_day)) {
        return fail(result,
                    EdfStrStorageErrorKind::Write,
                    "str_bad_date");
    }

    const size_t header_size = edf_str_header_size();
    uint8_t *actual_header = static_cast<uint8_t *>(
        Memory::alloc_large(header_size, false));
    uint8_t *expected_header = static_cast<uint8_t *>(
        Memory::alloc_large(header_size, false));
    uint8_t *scratch = static_cast<uint8_t *>(
        Memory::alloc_large(request.record_size * 2, false));
    if (!actual_header || !expected_header || !scratch) {
        Memory::free(actual_header);
        Memory::free(expected_header);
        Memory::free(scratch);
        return fail(result,
                    EdfStrStorageErrorKind::Allocation,
                    "str_work_alloc_failed");
    }
    uint8_t *incoming = scratch + request.record_size;

    bool ok = render_header(request, 0, expected_header, header_size);
    if (!ok) {
        fail(result,
             EdfStrStorageErrorKind::Write,
             "str_header_render_failed");
    }

    if (ok && !Storage::exists(request.path)) {
        ok = write_fresh_file(request, expected_header, result);
    } else if (ok) {
        File file = Storage::open(request.path, "r+");
        if (!file) {
            ok = fail(result,
                      EdfStrStorageErrorKind::Open,
                      "str_open_failed");
        } else {
            const size_t file_size = file.size();
            if (file_size < header_size ||
                !read_exact(file, 0, actual_header, header_size)) {
                file.close();
                ok = write_fresh_file(request, expected_header, result);
            } else if (!edf_str_header_schema_matches(actual_header,
                                                      expected_header,
                                                      header_size)) {
                file.close();
                ok = write_fresh_file(request, expected_header, result);
                if (ok) result.timeline_rewritten = true;
            } else {
                const size_t payload_size = file_size - header_size;
                const uint32_t record_count = static_cast<uint32_t>(
                    payload_size / request.record_size);
                const bool partial_tail =
                    (payload_size % request.record_size) != 0;

                uint32_t header_record_count = 0;
                const bool header_count_valid =
                    edf_resume_parse_record_count_field(actual_header,
                                                        header_size,
                                                        header_record_count);
                int32_t header_start_day = -1;
                const bool parsed_header_day = edf_str_header_start_day(
                    actual_header,
                    header_size,
                    header_start_day);
                bool header_day_valid = parsed_header_day;
                if (!header_day_valid) {
                    header_day_valid = infer_header_start_day(
                        file,
                        record_count,
                        scratch,
                        incoming_day,
                        header_start_day);
                }

                EdfStrTimelineScan scan;
                EdfStrTimelinePlan plan;
                ok = header_day_valid &&
                     scan_timeline(file,
                                   record_count,
                                   header_start_day,
                                   scratch,
                                   scan) &&
                     edf_str_timeline_plan(
                         scan,
                         incoming_day,
                         partial_tail || !parsed_header_day,
                         plan);
                if (!ok) {
                    file.close();
                    fail(result,
                         EdfStrStorageErrorKind::Read,
                         "str_timeline_scan_failed");
                } else if (plan.action == EdfStrTimelineAction::Rewrite) {
                    ok = rebuild_timeline(file,
                                          request,
                                          record_count,
                                          header_start_day,
                                          plan,
                                          actual_header,
                                          scratch,
                                          result);
                } else if (plan.action == EdfStrTimelineAction::Replace) {
                    ok = replace_existing_record(file,
                                                 plan.incoming_index,
                                                 request,
                                                 scratch,
                                                 incoming,
                                                 result);
                    result.record_count = record_count;
                } else {
                    ok = append_record(file,
                                       record_count,
                                       request,
                                       result);
                }

                if (ok && plan.action == EdfStrTimelineAction::Replace &&
                    (!header_count_valid ||
                     header_record_count != result.record_count)) {
                    ok = patch_record_count(file, result.record_count);
                    if (!ok) {
                        fail(result,
                             EdfStrStorageErrorKind::Write,
                             "str_count_patch_failed");
                    }
                }
                if (file) {
                    file.flush();
                    file.close();
                }
            }
        }
    }

    Memory::free(actual_header);
    Memory::free(expected_header);
    Memory::free(scratch);
    result.success = ok;
    return ok;
}

bool edf_str_storage_recover(const char *path,
                             bool &recovered,
                             const char *&error) {
    recovered = false;
    error = nullptr;

    char temporary[AC_STORAGE_PATH_MAX] = {};
    char backup[AC_STORAGE_PATH_MAX] = {};
    if (!companion_path(path, TEMP_SUFFIX, temporary) ||
        !companion_path(path, BACKUP_SUFFIX, backup)) {
        error = "str_rewrite_path_too_long";
        return false;
    }
    if (!recover_replacement(path, temporary, backup, recovered)) {
        error = "str_rewrite_recovery_failed";
        return false;
    }
    return true;
}

}  // namespace aircannect
