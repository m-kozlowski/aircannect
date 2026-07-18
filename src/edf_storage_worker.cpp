#include "edf_storage_worker.h"

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "debug_log.h"
#include "edf_file_reader.h"
#include "edf_file_resume.h"
#include "edf_identification.h"
#include "edf_storage_open_plan.h"
#include "edf_str_storage_writer.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace EdfStorageWorker {
namespace {

enum class JobType : uint8_t {
    Open,
    Record,
    NumericRecord,
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
    uint32_t request_id = 0;
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

    EdfFileSchema numeric_schema;
    EdfSignalSpec numeric_signals[AC_EDF_NUMERIC_SIGNAL_MAX + 1] = {};
    uint8_t numeric_source_indices[AC_EDF_NUMERIC_SIGNAL_MAX] = {};
    EdfSeriesId numeric_series = EdfSeriesId::Brp;
    uint32_t numeric_record_index = 0;
    size_t numeric_signal_count = 0;
    size_t numeric_samples_per_record = 0;
    float *numeric_values = nullptr;
    uint8_t *numeric_present = nullptr;
    uint8_t *numeric_valid = nullptr;
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

struct OpenRequestResult {
    bool complete = false;
    bool success = false;
    bool open = false;
    bool resumed = false;
    uint32_t request_id = 0;
    uint32_t record_count = 0;
    char path[sizeof(EdfStorageWorkerStatus::last_path)] = {};
    char error[sizeof(EdfStorageWorkerStatus::last_error)] = {};
};

void close_file(OpenFile &state);

EdfStorageWorkerStatus stats;
SemaphoreHandle_t queue_lock = nullptr;
TaskHandle_t task = nullptr;
JobSlot *slots = nullptr;
uint8_t *slot_bytes = nullptr;
float *slot_numeric_values = nullptr;
uint8_t *slot_numeric_present = nullptr;
uint8_t *slot_numeric_valid = nullptr;
size_t head = 0;
size_t tail = 0;
size_t queued = 0;
OpenFile open_files[AC_EDF_STORAGE_FILE_COUNT];
OpenRequestResult open_results[AC_EDF_STORAGE_FILE_COUNT];
uint32_t next_open_request_id = 0;
bool processing_job = false;

constexpr size_t max_size(size_t a, size_t b) {
    return a > b ? a : b;
}

static constexpr size_t AC_EDF_STORAGE_NUMERIC_VALUE_MAX = max_size(
    max_size(AC_EDF_BRP_SIGNAL_COUNT * AC_EDF_BRP_SAMPLES_PER_RECORD,
             AC_EDF_PLD_SIGNAL_COUNT * AC_EDF_PLD_SAMPLES_PER_RECORD),
    AC_EDF_SA2_SIGNAL_COUNT * AC_EDF_SA2_SAMPLES_PER_RECORD);

static constexpr size_t AC_EDF_STORAGE_NUMERIC_BIT_BYTES =
    (AC_EDF_STORAGE_NUMERIC_VALUE_MAX + 7u) / 8u;

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

bool legacy_str_backup_artifact_name(const char *name) {
    if (!name || strncmp(name, "STR-", 4) != 0) return false;
    const size_t len = strlen(name);
    return len > 8 && strcmp(name + len - 4, ".bak") == 0;
}

void recover_str_storage_artifacts() {
    if (!Storage::mounted()) return;

    Storage::Guard guard;
    bool recovered = false;
    const char *recovery_error = nullptr;
    if (!edf_str_storage_recover("/STR.edf", recovered, recovery_error)) {
        Log::logf(CAT_EDF,
                  LOG_ERROR,
                  "STR replacement recovery failed error=%s\n",
                  recovery_error ? recovery_error : "unknown");
    } else if (recovered) {
        Log::logf(CAT_EDF,
                  LOG_WARN,
                  "recovered interrupted STR replacement\n");
    }

    uint32_t removed = 0;
    File root = Storage::open("/", "r");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return;
    }
    for (;;) {
        File child = root.openNextFile();
        if (!child) break;
        const bool is_dir = child.isDirectory();
        const char *name = basename_from_path(child.name());
        char path[64] = {};
        const bool remove =
            !is_dir &&
            legacy_str_backup_artifact_name(name) &&
            build_child_path("/", name, path, sizeof(path));
        child.close();
        if (remove && Storage::remove(path)) removed++;
    }
    root.close();
    if (removed > 0) {
        Log::logf(CAT_EDF,
                  LOG_WARN,
                  "removed stale STR backup artifacts count=%lu\n",
                  static_cast<unsigned long>(removed));
    }
}

void set_error(const char *error) {
    copy_cstr(stats.last_error, sizeof(stats.last_error), error);
    stats.last_activity_ms = millis();
}

void log_alloc_failed(const char *context, size_t bytes) {
    Log::logf(CAT_EDF,
              LOG_ERROR,
              "storage allocation failed context=%s bytes=%u\n",
              context ? context : "--",
              static_cast<unsigned>(bytes));
}

void log_worker_failure(log_level_t level,
                        const char *error,
                        const char *path = nullptr) {
    Log::logf(CAT_EDF,
              level,
              "storage worker failure error=%s path=%s\n",
              error ? error : "--",
              path && path[0] ? path : "--");
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

EdfStorageFileIndex public_file_index(StoredFileKind kind) {
    switch (kind) {
        case StoredFileKind::Brp: return EdfStorageFileIndex::Brp;
        case StoredFileKind::Pld: return EdfStorageFileIndex::Pld;
        case StoredFileKind::Sa2: return EdfStorageFileIndex::Sa2;
        case StoredFileKind::Eve: return EdfStorageFileIndex::Eve;
        case StoredFileKind::Csl: return EdfStorageFileIndex::Csl;
        default: return EdfStorageFileIndex::Brp;
    }
}

size_t file_index(StoredFileKind kind) {
    return edf_storage_file_index(public_file_index(kind));
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

bool lock_queue(uint32_t timeout_ms = 10) {
    if (!queue_lock) return false;
    return xSemaphoreTake(queue_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlock_queue() {
    if (queue_lock) xSemaphoreGive(queue_lock);
}

EdfStorageOpenHandle reserve_open_handle(StoredFileKind kind) {
    EdfStorageOpenHandle handle;
    if (!lock_queue()) return handle;
    next_open_request_id++;
    if (next_open_request_id == 0) next_open_request_id++;
    handle.file = public_file_index(kind);
    handle.request_id = next_open_request_id;
    unlock_queue();
    return handle;
}

void store_open_result(const OpenRequestResult &result,
                       StoredFileKind kind) {
    const size_t index = file_index(kind);
    if (lock_queue(50)) {
        open_results[index] = result;
        unlock_queue();
        return;
    }
    open_results[index] = result;
}

void mark_open_result(const JobSlot &job,
                      bool success,
                      const OpenFile *state,
                      const char *error) {
    if (job.request_id == 0) return;

    OpenRequestResult result;
    result.complete = true;
    result.success = success;
    result.request_id = job.request_id;
    result.open = state && state->open;
    result.resumed = state && state->resumed;
    result.record_count = state ? state->record_count : 0;
    copy_cstr(result.path,
              sizeof(result.path),
              state && state->path[0] ? state->path : job.path);
    if (!success) {
        copy_cstr(result.error, sizeof(result.error), error);
    }
    store_open_result(result, job.kind);
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
    const size_t used = queued + (processing_job ? 1u : 0u);
    return AC_EDF_STORAGE_QUEUE_CAPACITY > used
               ? AC_EDF_STORAGE_QUEUE_CAPACITY - used
               : 0;
}

void clear_slot(JobSlot &slot) {
    uint8_t *bytes = slot.bytes;
    float *numeric_values = slot.numeric_values;
    uint8_t *numeric_present = slot.numeric_present;
    uint8_t *numeric_valid = slot.numeric_valid;
    slot = JobSlot{};
    slot.bytes = bytes;
    slot.numeric_values = numeric_values;
    slot.numeric_present = numeric_present;
    slot.numeric_valid = numeric_valid;
}

void free_slot_storage() {
    Memory::free(slots);
    Memory::free(slot_bytes);
    Memory::free(slot_numeric_values);
    Memory::free(slot_numeric_present);
    Memory::free(slot_numeric_valid);
    slots = nullptr;
    slot_bytes = nullptr;
    slot_numeric_values = nullptr;
    slot_numeric_present = nullptr;
    slot_numeric_valid = nullptr;
}

bool slot_storage_available() {
    return slots && slot_bytes && slot_numeric_values &&
           slot_numeric_present && slot_numeric_valid;
}

void bind_slot_storage(size_t index) {
    slots[index].bytes = slot_bytes + index * AC_EDF_STORAGE_SLOT_BYTES;
    slots[index].numeric_values =
        slot_numeric_values + index * AC_EDF_STORAGE_NUMERIC_VALUE_MAX;
    slots[index].numeric_present =
        slot_numeric_present + index * AC_EDF_STORAGE_NUMERIC_BIT_BYTES;
    slots[index].numeric_valid =
        slot_numeric_valid + index * AC_EDF_STORAGE_NUMERIC_BIT_BYTES;
}

bool allocate_slots() {
    if (slot_storage_available()) return true;
    slots = static_cast<JobSlot *>(Memory::calloc_large(
        AC_EDF_STORAGE_QUEUE_CAPACITY, sizeof(JobSlot), false));
    slot_bytes = static_cast<uint8_t *>(Memory::alloc_large(
        AC_EDF_STORAGE_SLOT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY, false));
    slot_numeric_values = static_cast<float *>(Memory::alloc_large(
        AC_EDF_STORAGE_NUMERIC_VALUE_MAX * AC_EDF_STORAGE_QUEUE_CAPACITY *
            sizeof(float),
        false));
    slot_numeric_present = static_cast<uint8_t *>(Memory::alloc_large(
        AC_EDF_STORAGE_NUMERIC_BIT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY,
        false));
    slot_numeric_valid = static_cast<uint8_t *>(Memory::alloc_large(
        AC_EDF_STORAGE_NUMERIC_BIT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY,
        false));
    if (!slot_storage_available()) {
        if (!slots) {
            log_alloc_failed("queue_slots",
                             AC_EDF_STORAGE_QUEUE_CAPACITY * sizeof(JobSlot));
        }
        if (!slot_bytes) {
            log_alloc_failed(
                "queue_payloads",
                AC_EDF_STORAGE_SLOT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY);
        }
        if (!slot_numeric_values) {
            log_alloc_failed("queue_numeric_values",
                             AC_EDF_STORAGE_NUMERIC_VALUE_MAX *
                                 AC_EDF_STORAGE_QUEUE_CAPACITY *
                                 sizeof(float));
        }
        if (!slot_numeric_present) {
            log_alloc_failed("queue_numeric_present",
                             AC_EDF_STORAGE_NUMERIC_BIT_BYTES *
                                 AC_EDF_STORAGE_QUEUE_CAPACITY);
        }
        if (!slot_numeric_valid) {
            log_alloc_failed("queue_numeric_valid",
                             AC_EDF_STORAGE_NUMERIC_BIT_BYTES *
                                 AC_EDF_STORAGE_QUEUE_CAPACITY);
        }
        free_slot_storage();
        set_error("allocation_failed");
        stats.available = false;
        return false;
    }
    for (size_t i = 0; i < AC_EDF_STORAGE_QUEUE_CAPACITY; ++i) {
        bind_slot_storage(i);
        clear_slot(slots[i]);
    }
    stats.using_psram = Memory::psram_available();
    stats.capacity = AC_EDF_STORAGE_QUEUE_CAPACITY;
    return true;
}

bool push_slot(const JobSlot &job) {
    if (!slots || queued >= AC_EDF_STORAGE_QUEUE_CAPACITY) return false;
    uint8_t *bytes = slots[tail].bytes;
    float *numeric_values = slots[tail].numeric_values;
    uint8_t *numeric_present = slots[tail].numeric_present;
    uint8_t *numeric_valid = slots[tail].numeric_valid;
    slots[tail] = job;
    slots[tail].bytes = bytes;
    slots[tail].numeric_values = numeric_values;
    slots[tail].numeric_present = numeric_present;
    slots[tail].numeric_valid = numeric_valid;
    tail = (tail + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
    queued++;
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
        log_alloc_failed("resume_headers", header_size * 2);
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
    if (!header) {
        log_alloc_failed("open_header", header_size);
        return false;
    }

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
    auto fail = [&](const char *error) {
        set_error(error);
        mark_open_result(job, false, nullptr, error);
        return false;
    };

    if (!valid_path(job.path)) {
        return fail("bad_path");
    }
    if (!Storage::mounted()) {
        stats.unavailable_drops++;
        return fail("storage_not_mounted");
    }

    Storage::Guard guard;
    if (!ensure_parent_dirs(job.path)) {
        return fail("mkdir_failed");
    }

    OpenFile &state = open_files[file_index(job.kind)];
    close_file(state);
    refresh_open_file_count();
    if (try_resume_open_file(state, job)) {
        refresh_open_file_count();
        mark_open_result(job, true, &state, nullptr);
        copy_cstr(stats.last_path, sizeof(stats.last_path), job.path);
        stats.open_jobs++;
        stats.last_error[0] = 0;
        return true;
    }

    (void)Storage::remove(job.path);
    state.file = Storage::open(job.path, "w+");
    if (!state.file) {
        stats.open_errors++;
        log_worker_failure(LOG_WARN, "open_failed", job.path);
        return fail("open_failed");
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
        log_worker_failure(LOG_WARN, "header_write_failed", job.path);
        return fail("header_write_failed");
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
        log_worker_failure(LOG_WARN, "recording_start_write_failed", job.path);
        return fail("recording_start_write_failed");
    }
    state.file.flush();
    refresh_open_file_count();
    mark_open_result(job, true, &state, nullptr);
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
        log_worker_failure(LOG_WARN, "file_not_open", job.path);
        return false;
    }
    if (state.record_size != 0 && job.len != state.record_size) {
        stats.write_errors++;
        set_error("record_size_mismatch");
        log_worker_failure(LOG_WARN, "record_size_mismatch", job.path);
        return false;
    }

    Storage::Guard guard;
    state.file.seek(state.file.size());
    const size_t written = state.file.write(job.bytes, job.len);
    if (written != job.len) {
        stats.write_errors++;
        set_error("short_write");
        Log::logf(CAT_EDF,
                  LOG_WARN,
                  "storage worker short write path=%s written=%u "
                  "expected=%u\n",
                  job.path[0] ? job.path : state.path,
                  static_cast<unsigned>(written),
                  static_cast<unsigned>(job.len));
        return false;
    }
    state.file.flush();
    state.record_count++;
    if (!patch_record_count(state)) {
        stats.patch_errors++;
        set_error("patch_failed");
        log_worker_failure(LOG_WARN, "patch_failed", state.path);
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

bool copy_numeric_schema(JobSlot &job, const EdfFileSchema &schema) {
    if (!schema.signals || schema.signal_count == 0 ||
        schema.signal_count > AC_EDF_NUMERIC_SIGNAL_MAX + 1 ||
        schema.source_signal_count > AC_EDF_NUMERIC_SIGNAL_MAX) {
        return false;
    }
    job.numeric_schema = schema;
    memcpy(job.numeric_signals,
           schema.signals,
           schema.signal_count * sizeof(EdfSignalSpec));
    job.numeric_schema.signals = job.numeric_signals;

    if (schema.source_signal_indices && schema.source_signal_count > 0) {
        memcpy(job.numeric_source_indices,
               schema.source_signal_indices,
               schema.source_signal_count * sizeof(uint8_t));
        job.numeric_schema.source_signal_indices =
            job.numeric_source_indices;
    } else {
        job.numeric_schema.source_signal_indices = nullptr;
    }
    return true;
}

bool copy_numeric_record(JobSlot &job,
                         const EdfCompletedRecordView &record) {
    if (!record.values || !record.present || !record.valid ||
        record.signal_count == 0 || record.samples_per_record == 0 ||
        !job.numeric_values || !job.numeric_present || !job.numeric_valid) {
        return false;
    }
    if (record.signal_count > SIZE_MAX / record.samples_per_record) {
        return false;
    }
    const size_t value_count =
        record.signal_count * record.samples_per_record;
    const size_t bit_bytes = (value_count + 7u) / 8u;
    if (value_count > AC_EDF_STORAGE_NUMERIC_VALUE_MAX ||
        bit_bytes > AC_EDF_STORAGE_NUMERIC_BIT_BYTES) {
        return false;
    }

    memcpy(job.numeric_values,
           record.values,
           value_count * sizeof(float));
    memcpy(job.numeric_present, record.present, bit_bytes);
    memcpy(job.numeric_valid, record.valid, bit_bytes);
    job.numeric_series = record.series;
    job.numeric_record_index = record.record_index;
    job.numeric_signal_count = record.signal_count;
    job.numeric_samples_per_record = record.samples_per_record;
    return true;
}

bool prepare_numeric_record_job(JobSlot &job,
                                const EdfFileSchema &schema,
                                const EdfCompletedRecordView &record) {
    const size_t record_size = edf_record_size(schema);
    if (record_size > AC_EDF_STORAGE_SLOT_BYTES) return false;
    if (!copy_numeric_schema(job, schema)) return false;
    if (!copy_numeric_record(job, record)) return false;
    job.type = JobType::NumericRecord;
    job.kind = stored_kind(schema.kind);
    job.record_size = record_size;
    job.len = record_size;
    return true;
}

bool process_numeric_record(JobSlot &job) {
    EdfCompletedRecordView record;
    record.series = job.numeric_series;
    record.record_index = job.numeric_record_index;
    record.signal_count = job.numeric_signal_count;
    record.samples_per_record = job.numeric_samples_per_record;
    record.values = job.numeric_values;
    record.present = job.numeric_present;
    record.valid = job.numeric_valid;

    size_t written = 0;
    if (!edf_render_numeric_record(job.numeric_schema,
                                   record,
                                   job.bytes,
                                   AC_EDF_STORAGE_SLOT_BYTES,
                                   written)) {
        stats.render_errors++;
        set_error("render_failed");
        log_worker_failure(LOG_WARN, "render_failed");
        return false;
    }
    job.len = written;
    return process_record(job);
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

    EdfStrStorageWriteRequest request;
    request.path = job.path;
    request.header.patient_id = job.patient_id;
    request.header.recording_id = job.recording_id;
    request.header.start_date = job.start_date;
    request.header.start_time = job.start_time;
    request.header.record_count = 0;
    request.record = job.bytes;
    request.record_size = job.len;

    EdfStrStorageWriteResult result;
    if (!edf_str_storage_write(request, result)) {
        if (result.error_kind == EdfStrStorageErrorKind::Allocation) {
            log_alloc_failed(result.error ? result.error : "str_write", 0);
        } else if (result.error_kind == EdfStrStorageErrorKind::Open) {
            stats.open_errors++;
        } else if (result.error_kind == EdfStrStorageErrorKind::Publish) {
            stats.patch_errors++;
        } else {
            stats.write_errors++;
        }
        set_error(result.error ? result.error : "str_write_failed");
        return false;
    }

    if (result.timeline_rewritten) {
        Log::logf(CAT_EDF,
                  result.retention_applied ? LOG_INFO : LOG_WARN,
                  "STR timeline rewritten records=%lu fillers=%lu "
                  "merged=%lu discarded=%lu retention=%u\n",
                  static_cast<unsigned long>(result.record_count),
                  static_cast<unsigned long>(result.filler_records),
                  static_cast<unsigned long>(result.merged_records),
                  static_cast<unsigned long>(result.discarded_records),
                  result.retention_applied ? 1u : 0u);
    }

    stats.records_written++;
    stats.bytes_written += result.bytes_written;
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
        log_worker_failure(LOG_WARN,
                           "identification_json_write_failed",
                           AC_EDF_IDENTIFICATION_JSON_PATH);
        return false;
    }

    uint8_t crc_le[4] = {};
    edf_identification_crc32_le(crc32_ieee(job.bytes, job.len), crc_le);
    if (!write_file_exact(AC_EDF_IDENTIFICATION_CRC_PATH,
                          crc_le,
                          sizeof(crc_le))) {
        stats.write_errors++;
        set_error("identification_crc_write_failed");
        log_worker_failure(LOG_WARN,
                           "identification_crc_write_failed",
                           AC_EDF_IDENTIFICATION_CRC_PATH);
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

void process_job(JobSlot &job) {
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
        case JobType::NumericRecord:
            (void)process_numeric_record(job);
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
    recover_str_storage_artifacts();
    stats.task_started = true;
    for (;;) {
        bool have_job = false;
        size_t slot_index = SIZE_MAX;
        if (lock_queue(50)) {
            if (slots && queued > 0 && !processing_job) {
                slot_index = head;
                head = (head + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
                queued--;
                stats.queued = queued;
                processing_job = true;
                have_job = true;
            }
            unlock_queue();
        }
        if (have_job) {
            process_job(slots[slot_index]);
            if (lock_queue(50)) {
                clear_slot(slots[slot_index]);
                processing_job = false;
                unlock_queue();
            } else {
                processing_job = false;
            }
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
        log_worker_failure(LOG_WARN, "queue_lock_failed", job.path);
        return false;
    }
    const bool ok = push_slot(job);
    unlock_queue();
    if (!ok) {
        stats.queue_drops++;
        set_error("queue_full");
        log_worker_failure(LOG_WARN, "queue_full", job.path);
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
        log_worker_failure(LOG_WARN, "queue_lock_failed");
        return false;
    }
    if (free_slots() == 0) {
        unlock_queue();
        stats.queue_drops++;
        set_error("queue_full");
        log_worker_failure(LOG_WARN, "queue_full");
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
        log_worker_failure(LOG_WARN, render_error, job.path);
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
            Log::logf(CAT_EDF, LOG_ERROR,
                      "storage worker task create failed\n");
            return;
        }
    }
    stats.initialized = true;
    stats.available = true;
    Log::logf(CAT_EDF, LOG_DEBUG,
              "storage worker ready q=%u slot=%u psram=%s\n",
              static_cast<unsigned>(AC_EDF_STORAGE_QUEUE_CAPACITY),
              static_cast<unsigned>(AC_EDF_STORAGE_SLOT_BYTES),
              stats.using_psram ? "yes" : "no");
}

bool enqueue_open_numeric(const char *path,
                          const EdfFileSchema &schema,
                          const EdfHeaderInfo &info,
                          EdfStorageOpenHandle *handle) {
    if (handle) *handle = {};
    if (!valid_path(path) || schema.signal_count == 0) return false;
    const size_t header_size = edf_header_size(schema);
    if (header_size > AC_EDF_STORAGE_SLOT_BYTES) return false;
    if (!stats.initialized) begin();
    if (!stats.available || !slots) return false;
    const StoredFileKind kind = stored_kind(schema.kind);
    const EdfStorageOpenHandle reserved = reserve_open_handle(kind);
    if (!reserved.valid()) return false;

    const bool queued = enqueue_rendered_slot(
        [&](JobSlot &job) {
            job.type = JobType::Open;
            job.kind = kind;
            job.request_id = reserved.request_id;
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
    if (!queued) return false;
    if (handle) *handle = reserved;
    return true;
}

bool enqueue_open_annotation(const char *path,
                             EdfAnnotationKind kind,
                             const EdfHeaderInfo &info,
                             EdfStorageOpenHandle *handle) {
    if (handle) *handle = {};
    if (!valid_path(path)) return false;
    if (!stats.initialized) begin();
    if (!stats.available || !slots) return false;
    const StoredFileKind stored = stored_kind(kind);
    const EdfStorageOpenHandle reserved = reserve_open_handle(stored);
    if (!reserved.valid()) return false;

    JobSlot job;
    job.type = JobType::Open;
    job.kind = stored;
    job.request_id = reserved.request_id;
    job.record_count = info.record_count;
    job.record_size = edf_annotation_record_size();
    job.recording_start = true;
    copy_cstr(job.path, sizeof(job.path), path);
    copy_cstr(job.patient_id, sizeof(job.patient_id), info.patient_id);
    copy_cstr(job.recording_id, sizeof(job.recording_id), info.recording_id);
    copy_cstr(job.start_date, sizeof(job.start_date), info.start_date);
    copy_cstr(job.start_time, sizeof(job.start_time), info.start_time);
    if (!enqueue(job)) return false;
    if (handle) *handle = reserved;
    return true;
}

bool enqueue_numeric_record(const EdfFileSchema &schema,
                            const EdfCompletedRecordView &record) {
    if (!stats.initialized) begin();
    if (!stats.available || !slots) return false;
    if (!lock_queue()) {
        stats.queue_drops++;
        set_error("queue_lock_failed");
        log_worker_failure(LOG_WARN, "queue_lock_failed");
        return false;
    }
    if (free_slots() == 0) {
        unlock_queue();
        stats.queue_drops++;
        set_error("queue_full");
        log_worker_failure(LOG_WARN, "queue_full");
        return false;
    }

    JobSlot &job = slots[tail];
    clear_slot(job);
    if (!prepare_numeric_record_job(job, schema, record)) {
        unlock_queue();
        stats.render_errors++;
        set_error("record_snapshot_failed");
        log_worker_failure(LOG_WARN, "record_snapshot_failed");
        return false;
    }

    const size_t queued_len = job.len;
    tail = (tail + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
    queued++;
    stats.queued = queued;
    unlock_queue();

    stats.bytes_enqueued += queued_len;
    stats.last_activity_ms = millis();
    return true;
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
    EdfStorageWorkerStatus out = stats;
    if (lock_queue()) {
        out = stats;
        out.queued = queued;
        out.busy = processing_job;
        unlock_queue();
    } else {
        out.busy = processing_job;
    }
#if AC_STACK_PROFILE_ENABLED
    if (task) out.stack_high_water_words = uxTaskGetStackHighWaterMark(task);
#endif
    return out;
}

#if AC_STACK_PROFILE_ENABLED
uint32_t stack_high_water_bytes() {
    return task ? uxTaskGetStackHighWaterMark(task) : 0;
}
#endif

bool open_result(const EdfStorageOpenHandle &handle,
                 EdfStorageOpenResult &result) {
    result = {};
    if (!handle.valid()) return false;
    const size_t index = edf_storage_file_index(handle.file);
    if (index >= AC_EDF_STORAGE_FILE_COUNT) return false;

    OpenRequestResult stored;
    if (lock_queue()) {
        stored = open_results[index];
        unlock_queue();
    } else {
        stored = open_results[index];
    }

    if (stored.request_id == handle.request_id && stored.complete) {
        result.complete = true;
        result.success = stored.success;
        result.open = stored.open;
        result.resumed = stored.resumed;
        result.record_count = stored.record_count;
        copy_cstr(result.path, sizeof(result.path), stored.path);
        copy_cstr(result.error, sizeof(result.error), stored.error);
        return true;
    }

    if (stored.request_id > handle.request_id) {
        result.complete = true;
        result.success = false;
        result.superseded = true;
        copy_cstr(result.error, sizeof(result.error), "open_superseded");
    }
    return true;
}

}  // namespace EdfStorageWorker
}  // namespace aircannect
