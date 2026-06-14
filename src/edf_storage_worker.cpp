#include "edf_storage_worker.h"

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "can_datagram.h"
#include "edf_file_reader.h"
#include "edf_file_resume.h"
#include "edf_identification.h"
#include "edf_storage_catalog.h"
#include "edf_storage_open_plan.h"
#include "edf_str_file_layout.h"
#include "edf_str_record_merge.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace EdfStorageWorker {
namespace {

enum class JobType : uint8_t {
    Open,
    Record,
    StrRecord,
    Identification,
    Close,
};

enum class StoredFileKind : uint8_t {
    Brp,
    Pld,
    Sa2,
    Eve,
    Csl,
};

struct JobSlot {
    JobType type = JobType::Record;
    StoredFileKind kind = StoredFileKind::Brp;
    char path[sizeof(EdfStorageWorkerStatus::last_path)] = {};
    char patient_id[AC_EDF_STORAGE_PATIENT_ID_MAX] = {};
    char recording_id[AC_EDF_STORAGE_RECORDING_ID_MAX] = {};
    char start_date[9] = {};
    char start_time[9] = {};
    uint32_t record_count = 0;
    uint8_t *bytes = nullptr;
    size_t len = 0;
    size_t record_size = 0;
    bool recording_start = false;
};

struct OpenFile {
    bool open = false;
    StoredFileKind kind = StoredFileKind::Brp;
    File file;
    uint32_t record_count = 0;
    size_t record_size = 0;
    bool resumed = false;
    char path[sizeof(EdfStorageWorkerStatus::last_path)] = {};
};

void close_file(OpenFile &state);

EdfStorageWorkerStatus stats;
SemaphoreHandle_t queue_lock = nullptr;
TaskHandle_t task = nullptr;
JobSlot *slots = nullptr;
uint8_t *slot_bytes = nullptr;
size_t head = 0;
size_t tail = 0;
size_t queued = 0;
OpenFile open_files[5];
bool processing_job = false;

const char *basename_from_path(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool build_child_path(const char *parent,
                      const char *name,
                      char *dst,
                      size_t dst_size) {
    if (!parent || !name || !*name || !dst || dst_size == 0) return false;
    const int written =
        strcmp(parent, "/") == 0
            ? snprintf(dst, dst_size, "/%s", name)
            : snprintf(dst, dst_size, "%s/%s", parent, name);
    return written > 0 && static_cast<size_t>(written) < dst_size;
}

void set_error(const char *error) {
    copy_cstr(stats.last_error, sizeof(stats.last_error), error);
    stats.last_activity_ms = millis();
}

StoredFileKind stored_kind(EdfFileKind kind) {
    switch (kind) {
        case EdfFileKind::Brp: return StoredFileKind::Brp;
        case EdfFileKind::Pld: return StoredFileKind::Pld;
        case EdfFileKind::Sa2: return StoredFileKind::Sa2;
        default: return StoredFileKind::Brp;
    }
}

StoredFileKind stored_kind(EdfAnnotationKind kind) {
    switch (kind) {
        case EdfAnnotationKind::Eve: return StoredFileKind::Eve;
        case EdfAnnotationKind::Csl: return StoredFileKind::Csl;
        default: return StoredFileKind::Eve;
    }
}

size_t file_index(StoredFileKind kind) {
    switch (kind) {
        case StoredFileKind::Brp: return 0;
        case StoredFileKind::Pld: return 1;
        case StoredFileKind::Sa2: return 2;
        case StoredFileKind::Eve: return 3;
        case StoredFileKind::Csl: return 4;
        default: return 0;
    }
}

EdfStorageOpenFileStatus open_file_status(const OpenFile &file) {
    EdfStorageOpenFileStatus out;
    out.open = file.open;
    out.resumed = file.resumed;
    out.record_count = file.record_count;
    copy_cstr(out.path, sizeof(out.path), file.path);
    return out;
}

EdfFileKind numeric_kind(StoredFileKind kind) {
    switch (kind) {
        case StoredFileKind::Pld: return EdfFileKind::Pld;
        case StoredFileKind::Sa2: return EdfFileKind::Sa2;
        case StoredFileKind::Brp:
        default:
            return EdfFileKind::Brp;
    }
}

bool is_annotation_kind(StoredFileKind kind) {
    return kind == StoredFileKind::Eve || kind == StoredFileKind::Csl;
}

size_t open_header_size(const JobSlot &job) {
    return is_annotation_kind(job.kind) ? edf_annotation_header_size()
                                        : job.len;
}

bool valid_path(const char *path) {
    if (!path || path[0] != '/') return false;
    const size_t len = strlen(path);
    return len > 1 && len < sizeof(EdfStorageWorkerStatus::last_path);
}

void fill_inventory_entry_base(EdfStorageInventoryEntry &entry,
                               const char *name,
                               const char *path,
                               bool directory,
                               uint64_t size) {
    entry = {};
    entry.directory = directory;
    entry.size = size;
    copy_cstr(entry.name, sizeof(entry.name), name);
    copy_cstr(entry.path, sizeof(entry.path), path);
}

void mark_inventory_file_status(EdfStorageInventoryEntry &entry,
                                EdfInventoryStatus status,
                                size_t file_size = 0,
                                uint32_t header_size = 0) {
    if (!edf_inventory_describe_path(entry.path, entry.file)) {
        entry.file = {};
    }
    entry.file.status = status;
    entry.file.file_size = file_size;
    entry.file.header.header_size = header_size;
}

void describe_inventory_file_locked(EdfStorageInventoryEntry &entry,
                                    File &file) {
    const size_t file_size = static_cast<size_t>(file.size());
    entry.size = file_size;

    uint8_t fixed_header[AC_EDF_HEADER_SIGNAL_HEADER_OFFSET] = {};
    if (file_size < sizeof(fixed_header)) {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::FileTooSmall,
                                   file_size);
        return;
    }
    if (!file.seek(0) ||
        file.read(fixed_header, sizeof(fixed_header)) !=
            sizeof(fixed_header)) {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::InvalidHeader,
                                   file_size);
        return;
    }

    uint32_t header_size = 0;
    if (!edf_parse_header_declared_size(fixed_header,
                                        sizeof(fixed_header),
                                        header_size)) {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::InvalidHeader,
                                   file_size);
        return;
    }
    if (header_size > AC_EDF_STORAGE_INVENTORY_HEADER_MAX) {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::InvalidHeader,
                                   file_size,
                                   header_size);
        return;
    }
    if (file_size < header_size) {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::FileTooSmall,
                                   file_size,
                                   header_size);
        return;
    }

    uint8_t *header = static_cast<uint8_t *>(
        Memory::alloc_large(header_size, true));
    if (!header) {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::InvalidHeader,
                                   file_size,
                                   header_size);
        return;
    }
    memcpy(header, fixed_header, sizeof(fixed_header));
    const size_t rest = header_size - sizeof(fixed_header);
    const bool read_ok =
        rest == 0 ||
        file.read(header + sizeof(fixed_header), rest) == rest;
    if (read_ok) {
        (void)edf_inventory_describe_file(entry.path,
                                          header,
                                          header_size,
                                          file_size,
                                          entry.file);
    } else {
        mark_inventory_file_status(entry,
                                   EdfInventoryStatus::InvalidHeader,
                                   file_size,
                                   header_size);
    }
    Memory::free(header);
}

bool visit_inventory_entry(const EdfStorageInventoryEntry &entry,
                           EdfStorageInventoryVisitor visitor,
                           void *ctx,
                           size_t offset,
                           size_t limit,
                           EdfStorageInventoryResult &result) {
    if (result.matched++ < offset) return true;
    if (result.returned >= limit) {
        result.truncated = true;
        return false;
    }
    if (!visitor(entry, ctx)) {
        result.status = EdfStorageInventoryStatus::VisitorStopped;
        return false;
    }
    ++result.returned;
    return true;
}

void root_datalog_inventory_entry(EdfStorageInventoryEntry &entry,
                                  bool &found) {
    found = false;
    Storage::Guard guard;
    File datalog = Storage::open("/DATALOG", "r");
    if (datalog && datalog.isDirectory()) {
        fill_inventory_entry_base(entry, "DATALOG", "/DATALOG", true, 0);
        found = true;
    }
    if (datalog) datalog.close();
}

void root_str_inventory_entry(EdfStorageInventoryEntry &entry,
                              bool &found) {
    found = false;
    Storage::Guard guard;
    File str = Storage::open("/STR.edf", "r");
    if (str && !str.isDirectory()) {
        fill_inventory_entry_base(entry,
                                  "STR.edf",
                                  "/STR.edf",
                                  false,
                                  str.size());
        describe_inventory_file_locked(entry, str);
        found = true;
    }
    if (str) str.close();
}

void root_metadata_inventory_entry(const char *path,
                                   const char *name,
                                   EdfStorageInventoryEntry &entry,
                                   bool &found) {
    found = false;
    Storage::Guard guard;
    File file = Storage::open(path, "r");
    if (file && !file.isDirectory()) {
        const size_t size = static_cast<size_t>(file.size());
        fill_inventory_entry_base(entry, name, path, false, size);
        (void)edf_inventory_describe_file(path, nullptr, 0, size, entry.file);
        found = true;
    }
    if (file) file.close();
}

bool open_inventory_directory(const char *path,
                              File &dir,
                              EdfStorageInventoryResult &result) {
    Storage::Guard guard;
    dir = Storage::open(path, "r");
    if (!dir) {
        result.status = EdfStorageInventoryStatus::NotFound;
        return false;
    }
    if (!dir.isDirectory()) {
        dir.close();
        result.status = EdfStorageInventoryStatus::NotDirectory;
        return false;
    }
    return true;
}

bool read_inventory_directory_child(const char *path,
                                    File &dir,
                                    EdfStorageInventoryEntry &entry,
                                    bool &have_child,
                                    bool &include) {
    have_child = false;
    include = false;
    Storage::Guard guard;
    File child = dir.openNextFile();
    if (!child) return true;
    have_child = true;

    const bool child_dir = child.isDirectory();
    const char *name = basename_from_path(child.name());
    char child_path[AC_EDF_STORAGE_INVENTORY_PATH_MAX] = {};

    if (build_child_path(path, name, child_path, sizeof(child_path))) {
        if (strcmp(path, "/DATALOG") == 0) {
            include = child_dir && edf_valid_browse_path(child_path);
        } else {
            include = !child_dir && edf_valid_pull_path(child_path);
        }
    }

    if (include) {
        fill_inventory_entry_base(entry,
                                  name,
                                  child_path,
                                  child_dir,
                                  child_dir ? 0 : child.size());
        if (!child_dir) describe_inventory_file_locked(entry, child);
    }
    child.close();
    return true;
}

void close_inventory_directory(File &dir) {
    Storage::Guard guard;
    if (dir) dir.close();
}

bool lock_queue(uint32_t timeout_ms = 10) {
    if (!queue_lock) return false;
    return xSemaphoreTake(queue_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlock_queue() {
    if (queue_lock) xSemaphoreGive(queue_lock);
}

uint8_t count_open_files() {
    uint8_t count = 0;
    for (const OpenFile &file : open_files) {
        if (file.open) ++count;
    }
    return count;
}

void refresh_open_file_count() {
    const uint8_t count = count_open_files();
    EdfStorageOpenFileStatus files[AC_EDF_STORAGE_FILE_COUNT];
    for (size_t i = 0; i < AC_EDF_STORAGE_FILE_COUNT; ++i) {
        files[i] = open_file_status(open_files[i]);
    }
    if (lock_queue(50)) {
        stats.open_file_count = count;
        for (size_t i = 0; i < AC_EDF_STORAGE_FILE_COUNT; ++i) {
            stats.files[i] = files[i];
        }
        unlock_queue();
        return;
    }
    stats.open_file_count = count;
    for (size_t i = 0; i < AC_EDF_STORAGE_FILE_COUNT; ++i) {
        stats.files[i] = files[i];
    }
}

size_t free_slots() {
    return AC_EDF_STORAGE_QUEUE_CAPACITY > queued
               ? AC_EDF_STORAGE_QUEUE_CAPACITY - queued
               : 0;
}

void clear_slot(JobSlot &slot) {
    slot.type = JobType::Record;
    slot.kind = StoredFileKind::Brp;
    slot.path[0] = 0;
    slot.patient_id[0] = 0;
    slot.recording_id[0] = 0;
    slot.start_date[0] = 0;
    slot.start_time[0] = 0;
    slot.record_count = 0;
    slot.len = 0;
    slot.record_size = 0;
    slot.recording_start = false;
}

bool allocate_slots() {
    if (slots && slot_bytes) return true;
    slots = static_cast<JobSlot *>(Memory::calloc_large(
        AC_EDF_STORAGE_QUEUE_CAPACITY, sizeof(JobSlot), false));
    slot_bytes = static_cast<uint8_t *>(Memory::alloc_large(
        AC_EDF_STORAGE_SLOT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY, false));
    if (!slots || !slot_bytes) {
        Memory::free(slots);
        Memory::free(slot_bytes);
        slots = nullptr;
        slot_bytes = nullptr;
        set_error("allocation_failed");
        stats.available = false;
        return false;
    }
    for (size_t i = 0; i < AC_EDF_STORAGE_QUEUE_CAPACITY; ++i) {
        slots[i].bytes = slot_bytes + i * AC_EDF_STORAGE_SLOT_BYTES;
    }
    stats.using_psram = Memory::psram_available();
    stats.capacity = AC_EDF_STORAGE_QUEUE_CAPACITY;
    return true;
}

bool push_slot(const JobSlot &job) {
    if (!slots || queued >= AC_EDF_STORAGE_QUEUE_CAPACITY) return false;
    uint8_t *bytes = slots[tail].bytes;
    slots[tail] = job;
    slots[tail].bytes = bytes;
    tail = (tail + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
    queued++;
    stats.queued = queued;
    return true;
}

bool pop_slot(JobSlot &job) {
    if (!slots || queued == 0) return false;
    job = slots[head];
    clear_slot(slots[head]);
    head = (head + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
    queued--;
    stats.queued = queued;
    return true;
}

bool ensure_parent_dirs(const char *path) {
    if (!valid_path(path)) return false;
    char dir[sizeof(EdfStorageWorkerStatus::last_path)] = {};
    copy_cstr(dir, sizeof(dir), path);
    char *last_slash = strrchr(dir, '/');
    if (!last_slash) return false;
    if (last_slash == dir) return true;
    *last_slash = 0;
    char *second_slash = strchr(dir + 1, '/');
    if (second_slash) {
        *second_slash = 0;
        if (!Storage::ensure_dir(dir)) return false;
        *second_slash = '/';
    }
    return Storage::ensure_dir(dir);
}

bool patch_record_count(OpenFile &state) {
    if (!state.open || !state.file) return false;
    char field[AC_EDF_HEADER_RECORD_COUNT_WIDTH] = {};
    memset(field, ' ', sizeof(field));
    char text[16] = {};
    snprintf(text, sizeof(text), "%lu",
             static_cast<unsigned long>(state.record_count));
    const size_t len = strlen(text);
    if (len > sizeof(field)) return false;
    memcpy(field, text, len);
    if (!state.file.seek(AC_EDF_HEADER_RECORD_COUNT_OFFSET)) return false;
    const size_t written = state.file.write(
        reinterpret_cast<const uint8_t *>(field), sizeof(field));
    if (written != sizeof(field)) return false;
    state.file.flush();
    state.file.seek(state.file.size());
    return true;
}

bool render_resume_header(const JobSlot &job,
                          uint8_t *dst,
                          size_t header_size) {
    if (!dst || header_size < AC_EDF_HEADER_SIGNAL_HEADER_OFFSET) return false;
    if (!is_annotation_kind(job.kind)) {
        if (!job.bytes || job.len != header_size) return false;
        memcpy(dst, job.bytes, header_size);
        return true;
    }

    EdfHeaderInfo info;
    info.patient_id = job.patient_id;
    info.recording_id = job.recording_id;
    info.start_date = job.start_date;
    info.start_time = job.start_time;
    info.record_count = 0;
    size_t written = 0;
    return edf_render_annotation_header(info, dst, header_size, written) &&
           written == header_size;
}

bool try_resume_open_file(OpenFile &state, const JobSlot &job) {
    const size_t header_size = open_header_size(job);
    if (!valid_path(job.path) ||
        header_size < AC_EDF_HEADER_SIGNAL_HEADER_OFFSET ||
        header_size > SIZE_MAX / 2 ||
        job.record_size == 0 || !Storage::exists(job.path)) {
        return false;
    }

    state.file = Storage::open(job.path, "r+");
    if (!state.file) return false;

    const size_t file_size = state.file.size();
    if (file_size < header_size) {
        state.file.close();
        return false;
    }
    uint8_t *headers = static_cast<uint8_t *>(
        Memory::alloc_large(header_size * 2, false));
    if (!headers) {
        state.file.close();
        return false;
    }
    uint8_t *actual = headers;
    uint8_t *expected = headers + header_size;

    bool ok = state.file.seek(0) &&
              state.file.read(actual, header_size) == header_size &&
              render_resume_header(job, expected, header_size);
    const EdfResumeDecision resume =
        ok ? edf_resume_check_file(actual,
                                   expected,
                                   header_size,
                                   file_size,
                                   job.record_size)
           : EdfResumeDecision{};
    Memory::free(headers);
    EdfStorageOpenPlanRequest plan_request;
    plan_request.annotation = is_annotation_kind(job.kind);
    plan_request.recording_start_requested = job.recording_start;
    plan_request.requested_record_count = job.record_count;
    plan_request.resume = resume;
    const EdfStorageOpenPlan plan = edf_storage_plan_open(plan_request);
    if (!ok || !plan.resume_existing) {
        state.file.close();
        return false;
    }

    state.kind = job.kind;
    state.record_count = plan.record_count;
    state.record_size = job.record_size;
    state.resumed = true;
    copy_cstr(state.path, sizeof(state.path), job.path);
    state.open = true;
    if (plan.patch_header_record_count && !patch_record_count(state)) {
        state.file.close();
        state.open = false;
        state.record_count = 0;
        state.record_size = 0;
        state.resumed = false;
        state.path[0] = 0;
        return false;
    }
    state.file.seek(state.file.size());
    return true;
}

bool write_header(OpenFile &state, const JobSlot &job) {
    const bool annotation = is_annotation_kind(job.kind);
    const EdfFileSchema *numeric_schema =
        annotation ? nullptr : &edf_numeric_schema(numeric_kind(job.kind));
    const size_t header_size =
        annotation ? edf_annotation_header_size()
                   : edf_header_size(*numeric_schema);
    uint8_t *header = static_cast<uint8_t *>(
        Memory::alloc_large(header_size, false));
    if (!header) return false;

    EdfHeaderInfo info;
    info.patient_id = job.patient_id;
    info.recording_id = job.recording_id;
    info.start_date = job.start_date;
    info.start_time = job.start_time;
    info.record_count = job.record_count;
    size_t written = 0;
    const bool rendered =
        annotation ? edf_render_annotation_header(info, header,
                                                  header_size, written)
                   : edf_render_header(*numeric_schema, info, header,
                                       header_size, written);
    bool ok = false;
    if (rendered && written == header_size) {
        ok = state.file.write(header, header_size) == header_size;
    }
    Memory::free(header);
    return ok;
}

bool write_open_header(OpenFile &state, const JobSlot &job) {
    if (!is_annotation_kind(job.kind) && job.len > 0) {
        return state.file.write(job.bytes, job.len) == job.len;
    }
    return write_header(state, job);
}

bool render_str_header_bytes(const JobSlot &job,
                             uint8_t *header,
                             size_t header_size) {
    if (!header || header_size != edf_str_header_size()) return false;
    EdfHeaderInfo info;
    info.patient_id = job.patient_id;
    info.recording_id = job.recording_id;
    info.start_date = job.start_date;
    info.start_time = job.start_time;
    info.record_count = job.record_count;
    size_t written = 0;
    return edf_render_str_header(info, header, header_size, written) &&
           written == header_size;
}

bool write_str_header(File &file, const JobSlot &job) {
    const size_t header_size = edf_str_header_size();
    uint8_t *header = static_cast<uint8_t *>(
        Memory::alloc_large(header_size, false));
    if (!header) return false;

    bool ok = false;
    if (render_str_header_bytes(job, header, header_size)) {
        ok = file.write(header, header_size) == header_size;
    }
    Memory::free(header);
    return ok;
}

bool read_str_header(File &file, uint8_t *header, size_t header_size) {
    return header && header_size == edf_str_header_size() &&
           file.seek(0) &&
           file.read(header, header_size) == header_size;
}

void collect_digits6(const char *src, char (&dst)[7]) {
    size_t out = 0;
    if (src) {
        for (size_t i = 0; src[i] && out < 6; ++i) {
            if (src[i] >= '0' && src[i] <= '9') {
                dst[out++] = src[i];
            }
        }
    }
    while (out < 6) dst[out++] = '0';
    dst[6] = 0;
}

bool build_str_backup_path(const JobSlot &job, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) return false;
    char date[7] = {};
    char time[7] = {};
    collect_digits6(job.start_date, date);
    collect_digits6(job.start_time, time);
    for (unsigned suffix = 0; suffix < 16; ++suffix) {
        const int n = suffix == 0
                          ? snprintf(dst, dst_size, "/STR-%s-%s.bak",
                                     date, time)
                          : snprintf(dst, dst_size, "/STR-%s-%s-%u.bak",
                                     date, time, suffix);
        if (n <= 0 || static_cast<size_t>(n) >= dst_size) return false;
        if (!Storage::exists(dst)) return true;
    }
    dst[0] = 0;
    return false;
}

bool patch_str_record_count(File &file, uint32_t record_count) {
    char field[AC_EDF_HEADER_RECORD_COUNT_WIDTH] = {};
    if (!edf_str_format_record_count_field(record_count,
                                           field,
                                           sizeof(field))) {
        return false;
    }
    if (!file.seek(AC_EDF_HEADER_RECORD_COUNT_OFFSET)) return false;
    return file.write(reinterpret_cast<const uint8_t *>(field),
                      sizeof(field)) == sizeof(field);
}

bool find_str_record_match(File &file,
                           uint32_t record_count,
                           int16_t date_sample,
                           EdfStrRecordMatch &match) {
    match = {};
    uint8_t raw[2] = {};
    for (uint32_t i = 0; i < record_count; ++i) {
        const size_t pos = edf_str_record_offset(i);
        if (!file.seek(pos)) return false;
        if (file.read(raw, sizeof(raw)) != sizeof(raw)) return false;
        if (edf_str_record_date_sample(raw, sizeof(raw)) == date_sample) {
            match.found = true;
            match.index = i;
            return true;
        }
    }
    return true;
}

void close_file(OpenFile &state) {
    if (!state.open) return;
    if (state.file) {
        (void)patch_record_count(state);
        state.file.flush();
        state.file.close();
    }
    state.open = false;
    state.record_count = 0;
    state.record_size = 0;
    state.resumed = false;
    state.path[0] = 0;
}

bool write_recording_start(OpenFile &state) {
    uint8_t record[64] = {};
    size_t written = 0;
    if (!edf_render_recording_start_annotation(record,
                                               sizeof(record),
                                               written) ||
        written != state.record_size) {
        return false;
    }
    if (state.file.write(record, written) != written) return false;
    state.record_count++;
    if (!patch_record_count(state)) return false;
    stats.records_written++;
    stats.bytes_written += written;
    return true;
}

bool process_open(const JobSlot &job) {
    if (!valid_path(job.path)) {
        set_error("bad_path");
        return false;
    }
    if (!Storage::mounted()) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        return false;
    }

    Storage::Guard guard;
    if (!ensure_parent_dirs(job.path)) {
        set_error("mkdir_failed");
        return false;
    }

    OpenFile &state = open_files[file_index(job.kind)];
    close_file(state);
    refresh_open_file_count();
    if (try_resume_open_file(state, job)) {
        refresh_open_file_count();
        copy_cstr(stats.last_path, sizeof(stats.last_path), job.path);
        stats.open_jobs++;
        stats.last_error[0] = 0;
        return true;
    }

    (void)Storage::remove(job.path);
    state.file = Storage::open(job.path, "w+");
    if (!state.file) {
        stats.open_errors++;
        set_error("open_failed");
        return false;
    }
    state.kind = job.kind;
    EdfStorageOpenPlanRequest plan_request;
    plan_request.annotation = is_annotation_kind(job.kind);
    plan_request.recording_start_requested = job.recording_start;
    plan_request.requested_record_count = job.record_count;
    const EdfStorageOpenPlan plan = edf_storage_plan_open(plan_request);
    state.record_count = plan.record_count;
    state.record_size = job.record_size;
    state.resumed = false;
    copy_cstr(state.path, sizeof(state.path), job.path);
    if (!write_open_header(state, job)) {
        state.file.close();
        stats.write_errors++;
        set_error("header_write_failed");
        return false;
    }
    state.open = true;
    if (plan.write_recording_start && !write_recording_start(state)) {
        state.file.close();
        state.open = false;
        state.record_count = 0;
        state.record_size = 0;
        state.resumed = false;
        state.path[0] = 0;
        stats.write_errors++;
        set_error("recording_start_write_failed");
        return false;
    }
    state.file.flush();
    refresh_open_file_count();
    copy_cstr(stats.last_path, sizeof(stats.last_path), job.path);
    stats.open_jobs++;
    stats.last_error[0] = 0;
    return true;
}

bool process_record(const JobSlot &job) {
    OpenFile &state = open_files[file_index(job.kind)];
    if (!state.open || !state.file) {
        stats.write_errors++;
        set_error("file_not_open");
        return false;
    }
    if (state.record_size != 0 && job.len != state.record_size) {
        stats.write_errors++;
        set_error("record_size_mismatch");
        return false;
    }

    Storage::Guard guard;
    state.file.seek(state.file.size());
    const size_t written = state.file.write(job.bytes, job.len);
    if (written != job.len) {
        stats.write_errors++;
        set_error("short_write");
        return false;
    }
    state.file.flush();
    state.record_count++;
    if (!patch_record_count(state)) {
        stats.patch_errors++;
        set_error("patch_failed");
        return false;
    }
    stats.records_written++;
    stats.bytes_written += written;
    stats.record_jobs++;
    stats.last_activity_ms = millis();
    copy_cstr(stats.last_path, sizeof(stats.last_path), state.path);
    stats.last_error[0] = 0;
    return true;
}

bool process_str_record(const JobSlot &job) {
    if (!valid_path(job.path)) {
        set_error("bad_str_path");
        return false;
    }
    if (!Storage::mounted()) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        return false;
    }

    Storage::Guard guard;
    if (!ensure_parent_dirs(job.path)) {
        set_error("str_mkdir_failed");
        return false;
    }

    const bool existed = Storage::exists(job.path);
    File file = Storage::open(job.path, existed ? "r+" : "w+");
    if (!file) {
        stats.open_errors++;
        set_error("str_open_failed");
        return false;
    }

    uint32_t record_count = 0;
    const size_t initial_size = file.size();
    if (!existed || initial_size < edf_str_header_size()) {
        if (!write_str_header(file, job)) {
            file.close();
            stats.write_errors++;
            set_error("str_header_write_failed");
            return false;
        }
        file.flush();
    } else {
        EdfStrFileLayout layout;
        if (!edf_str_file_layout_from_size(initial_size, layout)) {
            file.close();
            stats.write_errors++;
            set_error("str_partial_tail");
            return false;
        }
        record_count = layout.record_count;

        const size_t header_size = edf_str_header_size();
        uint8_t *actual_header = static_cast<uint8_t *>(
            Memory::alloc_large(header_size, false));
        uint8_t *expected_header = static_cast<uint8_t *>(
            Memory::alloc_large(header_size, false));
        const bool headers_available =
            actual_header && expected_header &&
            read_str_header(file, actual_header, header_size) &&
            render_str_header_bytes(job, expected_header, header_size);
        const bool schema_matches =
            headers_available &&
            edf_str_header_schema_matches(actual_header,
                                          expected_header,
                                          header_size);
        uint32_t header_record_count = 0;
        const bool header_count_valid =
            headers_available &&
            edf_resume_parse_record_count_field(actual_header,
                                                header_size,
                                                header_record_count);
        Memory::free(actual_header);
        Memory::free(expected_header);

        if (!headers_available) {
            file.close();
            stats.write_errors++;
            set_error("str_header_read_failed");
            return false;
        }

        if (!schema_matches) {
            char backup_path[64] = {};
            if (!build_str_backup_path(job, backup_path,
                                       sizeof(backup_path))) {
                file.close();
                stats.write_errors++;
                set_error("str_backup_path_failed");
                return false;
            }
            file.close();
            if (!Storage::rename(job.path, backup_path)) {
                stats.write_errors++;
                set_error("str_backup_failed");
                return false;
            }
            Log::logf(CAT_STREAM, LOG_WARN,
                      "[EDF] rotated incompatible STR root file to %s\n",
                      backup_path);

            file = Storage::open(job.path, "w+");
            if (!file) {
                stats.open_errors++;
                set_error("str_reopen_failed");
                return false;
            }
            record_count = 0;
            if (!write_str_header(file, job)) {
                file.close();
                stats.write_errors++;
                set_error("str_header_write_failed");
                return false;
            }
            file.flush();
        } else if (!header_count_valid ||
                   header_record_count != record_count) {
            if (!patch_str_record_count(file, record_count)) {
                file.close();
                stats.patch_errors++;
                set_error("str_count_patch_failed");
                return false;
            }
            file.flush();
        }
    }

    const int16_t date_sample = edf_str_record_date_sample(job.bytes,
                                                           job.len);
    if (!edf_str_date_sample_valid(date_sample)) {
        file.close();
        stats.write_errors++;
        set_error("str_bad_date");
        return false;
    }

    EdfStrRecordMatch match;
    if (!find_str_record_match(file, record_count, date_sample, match)) {
        file.close();
        stats.write_errors++;
        set_error("str_scan_failed");
        return false;
    }

    EdfStrRecordLocation location;
    if (!edf_str_resolve_record_location(record_count, match, location)) {
        file.close();
        stats.write_errors++;
        set_error("str_location_failed");
        return false;
    }

    const uint8_t *record_bytes = job.bytes;
    uint8_t merged_record[AC_EDF_STR_SAMPLES_PER_RECORD * 2] = {};
    if (match.found) {
        const size_t existing_offset = edf_str_record_offset(match.index);
        uint8_t existing_record[AC_EDF_STR_SAMPLES_PER_RECORD * 2] = {};
        if (!file.seek(existing_offset) ||
            file.read(existing_record, sizeof(existing_record)) !=
                sizeof(existing_record)) {
            file.close();
            stats.write_errors++;
            set_error("str_existing_read_failed");
            return false;
        }
        memcpy(merged_record, job.bytes, sizeof(merged_record));
        const EdfStrRecordMergeStatus merge_status =
            edf_str_merge_existing_record(existing_record,
                                          sizeof(existing_record),
                                          merged_record,
                                          sizeof(merged_record));
        if (merge_status != EdfStrRecordMergeStatus::Ok) {
            file.close();
            stats.write_errors++;
            char error[sizeof(stats.last_error)] = {};
            snprintf(error,
                     sizeof(error),
                     "str_merge_%s",
                     edf_str_record_merge_status_name(merge_status));
            set_error(error);
            return false;
        }
        record_bytes = merged_record;
    }

    if (!file.seek(location.offset)) {
        file.close();
        stats.write_errors++;
        set_error("str_seek_failed");
        return false;
    }
    const size_t written = file.write(record_bytes, job.len);
    if (written != job.len) {
        file.close();
        stats.write_errors++;
        set_error("str_short_write");
        return false;
    }
    file.flush();
    if (location.appending) {
        record_count++;
        if (!patch_str_record_count(file, record_count)) {
            file.close();
            stats.patch_errors++;
            set_error("str_patch_failed");
            return false;
        }
        file.flush();
    }
    file.close();

    stats.records_written++;
    stats.bytes_written += written;
    stats.record_jobs++;
    stats.last_activity_ms = millis();
    copy_cstr(stats.last_path, sizeof(stats.last_path), job.path);
    stats.last_error[0] = 0;
    return true;
}

bool write_file_exact(const char *path, const uint8_t *data, size_t len) {
    if (!valid_path(path) || (!data && len > 0)) return false;
    File file = Storage::open(path, "w");
    if (!file) return false;
    const size_t written = len > 0 ? file.write(data, len) : 0;
    file.flush();
    file.close();
    return written == len;
}

bool process_identification_files(const JobSlot &job) {
    if (!job.bytes || job.len == 0) {
        set_error("identification_empty");
        return false;
    }
    if (!Storage::mounted()) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        return false;
    }

    Storage::Guard guard;
    if (!write_file_exact(AC_EDF_IDENTIFICATION_JSON_PATH,
                          job.bytes,
                          job.len)) {
        stats.write_errors++;
        set_error("identification_json_write_failed");
        return false;
    }

    uint8_t crc_le[4] = {};
    edf_identification_crc32_le(crc32_ieee(job.bytes, job.len), crc_le);
    if (!write_file_exact(AC_EDF_IDENTIFICATION_CRC_PATH,
                          crc_le,
                          sizeof(crc_le))) {
        stats.write_errors++;
        set_error("identification_crc_write_failed");
        return false;
    }

    stats.identification_jobs++;
    stats.bytes_written += job.len + sizeof(crc_le);
    stats.last_activity_ms = millis();
    copy_cstr(stats.last_path,
              sizeof(stats.last_path),
              AC_EDF_IDENTIFICATION_JSON_PATH);
    stats.last_error[0] = 0;
    return true;
}

bool process_close(const JobSlot &job) {
    Storage::Guard guard;
    OpenFile &state = open_files[file_index(job.kind)];
    close_file(state);
    refresh_open_file_count();
    stats.close_jobs++;
    stats.last_activity_ms = millis();
    stats.last_error[0] = 0;
    return true;
}

void process_job(const JobSlot &job) {
    switch (job.type) {
        case JobType::Open:
            (void)process_open(job);
            break;
        case JobType::Close:
            (void)process_close(job);
            break;
        case JobType::Record:
            (void)process_record(job);
            break;
        case JobType::StrRecord:
            (void)process_str_record(job);
            break;
        case JobType::Identification:
            (void)process_identification_files(job);
            break;
        default:
            (void)process_record(job);
            break;
    }
}

void task_entry(void *) {
    stats.task_started = true;
    for (;;) {
        JobSlot job;
        bool have_job = false;
        if (lock_queue(50)) {
            have_job = pop_slot(job);
            if (have_job) processing_job = true;
            unlock_queue();
        }
        if (have_job) {
            process_job(job);
            processing_job = false;
            vTaskDelay(pdMS_TO_TICKS(AC_EDF_STORAGE_WORK_TICK_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(AC_EDF_STORAGE_IDLE_TICK_MS));
        }
    }
}

bool enqueue(JobSlot &job) {
    if (!stats.initialized) begin();
    if (!stats.available || !slots) return false;
    if (!lock_queue()) {
        stats.queue_drops++;
        set_error("queue_lock_failed");
        return false;
    }
    const bool ok = push_slot(job);
    unlock_queue();
    if (!ok) {
        stats.queue_drops++;
        set_error("queue_full");
        return false;
    }
    stats.bytes_enqueued += job.len;
    stats.last_activity_ms = millis();
    return true;
}

template <typename Prepare, typename Render>
bool enqueue_rendered_slot(Prepare prepare,
                           Render render,
                           const char *render_error) {
    if (!stats.initialized) begin();
    if (!stats.available || !slots) return false;
    if (!lock_queue()) {
        stats.queue_drops++;
        set_error("queue_lock_failed");
        return false;
    }
    if (free_slots() == 0) {
        unlock_queue();
        stats.queue_drops++;
        set_error("queue_full");
        return false;
    }

    JobSlot &job = slots[tail];
    clear_slot(job);
    prepare(job);

    size_t written = 0;
    if (!render(job, written)) {
        unlock_queue();
        stats.render_errors++;
        set_error(render_error);
        return false;
    }

    job.len = written;
    tail = (tail + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
    queued++;
    stats.queued = queued;
    unlock_queue();

    stats.bytes_enqueued += written;
    stats.last_activity_ms = millis();
    return true;
}

}  // namespace

void begin() {
    if (stats.initialized) return;
    if (!queue_lock) queue_lock = xSemaphoreCreateMutex();
    if (!queue_lock || !allocate_slots()) {
        stats.available = false;
        return;
    }
    if (!task) {
        const BaseType_t created =
            xTaskCreatePinnedToCore(task_entry, "ac_edf_storage",
                                    AC_EDF_STORAGE_TASK_STACK, nullptr,
                                    AC_EDF_STORAGE_TASK_PRIO, &task,
                                    AC_EDF_STORAGE_TASK_CORE);
        if (created != pdPASS || !task) {
            stats.available = false;
            set_error("task_create_failed");
            Log::logf(CAT_STREAM, LOG_ERROR,
                      "[EDF] storage worker task create failed\n");
            return;
        }
    }
    stats.initialized = true;
    stats.available = true;
    Log::logf(CAT_STREAM, LOG_INFO,
              "[EDF] storage worker ready q=%u slot=%u psram=%s\n",
              static_cast<unsigned>(AC_EDF_STORAGE_QUEUE_CAPACITY),
              static_cast<unsigned>(AC_EDF_STORAGE_SLOT_BYTES),
              stats.using_psram ? "yes" : "no");
}

bool enqueue_open_numeric(const char *path,
                          const EdfFileSchema &schema,
                          const EdfHeaderInfo &info) {
    if (!valid_path(path) || schema.signal_count == 0) return false;
    const size_t header_size = edf_header_size(schema);
    if (header_size > AC_EDF_STORAGE_SLOT_BYTES) return false;
    return enqueue_rendered_slot(
        [&](JobSlot &job) {
            job.type = JobType::Open;
            job.kind = stored_kind(schema.kind);
            job.record_count = info.record_count;
            job.record_size = edf_record_size(schema);
            copy_cstr(job.path, sizeof(job.path), path);
            copy_cstr(job.patient_id, sizeof(job.patient_id),
                      info.patient_id);
            copy_cstr(job.recording_id, sizeof(job.recording_id),
                      info.recording_id);
            copy_cstr(job.start_date, sizeof(job.start_date),
                      info.start_date);
            copy_cstr(job.start_time, sizeof(job.start_time),
                      info.start_time);
        },
        [&](JobSlot &job, size_t &written) {
            return edf_render_header(schema,
                                     info,
                                     job.bytes,
                                     AC_EDF_STORAGE_SLOT_BYTES,
                                     written) &&
                   written == header_size;
        },
        "header_render_failed");
}

bool enqueue_open_annotation(const char *path,
                             EdfAnnotationKind kind,
                             const EdfHeaderInfo &info) {
    if (!valid_path(path)) return false;
    JobSlot job;
    job.type = JobType::Open;
    job.kind = stored_kind(kind);
    job.record_count = info.record_count;
    job.record_size = edf_annotation_record_size();
    job.recording_start = true;
    copy_cstr(job.path, sizeof(job.path), path);
    copy_cstr(job.patient_id, sizeof(job.patient_id), info.patient_id);
    copy_cstr(job.recording_id, sizeof(job.recording_id), info.recording_id);
    copy_cstr(job.start_date, sizeof(job.start_date), info.start_date);
    copy_cstr(job.start_time, sizeof(job.start_time), info.start_time);
    return enqueue(job);
}

bool enqueue_numeric_record(const EdfFileSchema &schema,
                            const EdfCompletedRecordView &record) {
    if (edf_record_size(schema) > AC_EDF_STORAGE_SLOT_BYTES) return false;
    return enqueue_rendered_slot(
        [&](JobSlot &job) {
            job.type = JobType::Record;
            job.kind = stored_kind(schema.kind);
        },
        [&](JobSlot &job, size_t &written) {
            return edf_render_numeric_record(schema,
                                             record,
                                             job.bytes,
                                             AC_EDF_STORAGE_SLOT_BYTES,
                                             written);
        },
        "render_failed");
}

bool enqueue_annotation_record(EdfAnnotationKind kind,
                               const EdfAnnotationRecord &record) {
    if (edf_annotation_record_size() > AC_EDF_STORAGE_SLOT_BYTES) return false;
    return enqueue_rendered_slot(
        [&](JobSlot &job) {
            job.type = JobType::Record;
            job.kind = stored_kind(kind);
        },
        [&](JobSlot &job, size_t &written) {
            return edf_render_annotation_record(record,
                                                job.bytes,
                                                AC_EDF_STORAGE_SLOT_BYTES,
                                                written);
        },
        "annotation_render_failed");
}

bool enqueue_str_record(const char *path,
                        const EdfHeaderInfo &info,
                        const EdfStrRecordView &record) {
    if (!valid_path(path) || edf_str_record_size() > AC_EDF_STORAGE_SLOT_BYTES) {
        return false;
    }
    return enqueue_rendered_slot(
        [&](JobSlot &job) {
            job.type = JobType::StrRecord;
            copy_cstr(job.path, sizeof(job.path), path);
            copy_cstr(job.patient_id, sizeof(job.patient_id),
                      info.patient_id);
            copy_cstr(job.recording_id, sizeof(job.recording_id),
                      info.recording_id);
            copy_cstr(job.start_date, sizeof(job.start_date),
                      info.start_date);
            copy_cstr(job.start_time, sizeof(job.start_time),
                      info.start_time);
        },
        [&](JobSlot &job, size_t &written) {
            return edf_render_str_record(record,
                                         job.bytes,
                                         AC_EDF_STORAGE_SLOT_BYTES,
                                         written);
        },
        "str_render_failed");
}

bool enqueue_identification_files(const std::string &json) {
    if (json.empty() || json.size() > AC_EDF_STORAGE_SLOT_BYTES) {
        return false;
    }
    return enqueue_rendered_slot(
        [&](JobSlot &job) {
            job.type = JobType::Identification;
            copy_cstr(job.path,
                      sizeof(job.path),
                      AC_EDF_IDENTIFICATION_JSON_PATH);
        },
        [&](JobSlot &job, size_t &written) {
            memcpy(job.bytes, json.data(), json.size());
            written = json.size();
            return true;
        },
        "identification_render_failed");
}

bool enqueue_close_numeric(EdfFileKind kind) {
    JobSlot job;
    job.type = JobType::Close;
    job.kind = stored_kind(kind);
    return enqueue(job);
}

bool enqueue_close_annotation(EdfAnnotationKind kind) {
    JobSlot job;
    job.type = JobType::Close;
    job.kind = stored_kind(kind);
    return enqueue(job);
}

EdfStorageWorkerStatus status() {
    if (!stats.initialized) begin();
    EdfStorageWorkerStatus out = stats;
    if (lock_queue()) {
        out = stats;
        out.queued = queued;
        out.busy = processing_job;
        unlock_queue();
    } else {
        out.busy = processing_job;
    }
    if (task) {
        out.stack_high_water_words = uxTaskGetStackHighWaterMark(task);
    }
    return out;
}

EdfStorageInventoryResult list_inventory(
    const char *path,
    size_t offset,
    size_t limit,
    EdfStorageInventoryVisitor visitor,
    void *ctx) {
    EdfStorageInventoryResult result;
    if (!visitor || !edf_valid_browse_path(path)) {
        result.status = EdfStorageInventoryStatus::BadPath;
        return result;
    }
    if (!Storage::mounted()) {
        result.status = EdfStorageInventoryStatus::StorageUnavailable;
        return result;
    }

    const EdfStorageWorkerStatus worker = status();
    if (worker.busy || worker.queued > 0 || worker.open_file_count > 0) {
        result.status = EdfStorageInventoryStatus::Busy;
        return result;
    }

    if (strcmp(path, "/") == 0) {
        EdfStorageInventoryEntry entry;
        bool found = false;
        root_datalog_inventory_entry(entry, found);
        if (found) {
            if (!visit_inventory_entry(entry,
                                       visitor,
                                       ctx,
                                       offset,
                                       limit,
                                       result)) {
                return result;
            }
        }

        root_str_inventory_entry(entry, found);
        if (found) {
            if (!visit_inventory_entry(entry,
                                       visitor,
                                       ctx,
                                       offset,
                                       limit,
                                       result)) {
                return result;
            }
        }

        root_metadata_inventory_entry(AC_EDF_IDENTIFICATION_CRC_PATH,
                                      "Identification.crc",
                                      entry,
                                      found);
        if (found) {
            if (!visit_inventory_entry(entry,
                                       visitor,
                                       ctx,
                                       offset,
                                       limit,
                                       result)) {
                return result;
            }
        }

        root_metadata_inventory_entry(AC_EDF_IDENTIFICATION_JSON_PATH,
                                      "Identification.json",
                                      entry,
                                      found);
        if (found) {
            if (!visit_inventory_entry(entry,
                                       visitor,
                                       ctx,
                                       offset,
                                       limit,
                                       result)) {
                return result;
            }
        }
        return result;
    }

    File dir;
    if (!open_inventory_directory(path, dir, result)) return result;

    while (!result.truncated &&
           result.status == EdfStorageInventoryStatus::Ok) {
        EdfStorageInventoryEntry entry;
        bool have_child = false;
        bool include = false;
        if (!read_inventory_directory_child(path,
                                            dir,
                                            entry,
                                            have_child,
                                            include) ||
            !have_child) {
            break;
        }
        if (!include) continue;
        (void)visit_inventory_entry(entry,
                                    visitor,
                                    ctx,
                                    offset,
                                    limit,
                                    result);
    }
    close_inventory_directory(dir);
    return result;
}

const char *inventory_status_name(EdfStorageInventoryStatus status) {
    switch (status) {
        case EdfStorageInventoryStatus::Ok: return "ok";
        case EdfStorageInventoryStatus::BadPath: return "bad_path";
        case EdfStorageInventoryStatus::StorageUnavailable:
            return "storage_unavailable";
        case EdfStorageInventoryStatus::Busy: return "edf_storage_busy";
        case EdfStorageInventoryStatus::NotFound: return "not_found";
        case EdfStorageInventoryStatus::NotDirectory: return "not_directory";
        case EdfStorageInventoryStatus::VisitorStopped:
            return "visitor_stopped";
        default:
            return "unknown";
    }
}

}  // namespace EdfStorageWorker
}  // namespace aircannect
