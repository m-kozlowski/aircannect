#include "edf_storage_worker.h"

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"

namespace aircannect {
namespace EdfStorageWorker {
namespace {

enum class JobType : uint8_t {
    Open,
    Record,
    Close,
};

struct JobSlot {
    JobType type = JobType::Record;
    EdfFileKind kind = EdfFileKind::Brp;
    char path[sizeof(EdfStorageWorkerStatus::last_path)] = {};
    char patient_id[AC_EDF_STORAGE_PATIENT_ID_MAX] = {};
    char recording_id[AC_EDF_STORAGE_RECORDING_ID_MAX] = {};
    char start_date[9] = {};
    char start_time[9] = {};
    uint32_t record_count = 0;
    uint8_t *bytes = nullptr;
    size_t len = 0;
};

struct OpenFile {
    bool open = false;
    EdfFileKind kind = EdfFileKind::Brp;
    File file;
    uint32_t record_count = 0;
    char path[sizeof(EdfStorageWorkerStatus::last_path)] = {};
};

EdfStorageWorkerStatus stats;
SemaphoreHandle_t queue_lock = nullptr;
TaskHandle_t task = nullptr;
JobSlot *slots = nullptr;
uint8_t *slot_bytes = nullptr;
size_t head = 0;
size_t tail = 0;
size_t queued = 0;
OpenFile open_files[3];

void copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

void set_error(const char *error) {
    copy_text(stats.last_error, sizeof(stats.last_error), error);
    stats.last_activity_ms = millis();
}

size_t file_index(EdfFileKind kind) {
    switch (kind) {
        case EdfFileKind::Brp: return 0;
        case EdfFileKind::Pld: return 1;
        case EdfFileKind::Sa2: return 2;
        default: return 0;
    }
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

size_t free_slots() {
    return AC_EDF_STORAGE_QUEUE_CAPACITY > queued
               ? AC_EDF_STORAGE_QUEUE_CAPACITY - queued
               : 0;
}

void clear_slot(JobSlot &slot) {
    slot.type = JobType::Record;
    slot.kind = EdfFileKind::Brp;
    slot.path[0] = 0;
    slot.patient_id[0] = 0;
    slot.recording_id[0] = 0;
    slot.start_date[0] = 0;
    slot.start_time[0] = 0;
    slot.record_count = 0;
    slot.len = 0;
}

bool allocate_slots() {
    if (slots && slot_bytes) return true;
    slots = static_cast<JobSlot *>(Memory::alloc_internal(
        sizeof(JobSlot) * AC_EDF_STORAGE_QUEUE_CAPACITY));
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
    memset(slots, 0, sizeof(JobSlot) * AC_EDF_STORAGE_QUEUE_CAPACITY);
    for (size_t i = 0; i < AC_EDF_STORAGE_QUEUE_CAPACITY; ++i) {
        slots[i].bytes = slot_bytes + i * AC_EDF_STORAGE_SLOT_BYTES;
    }
    stats.using_psram = Memory::psram_available();
    stats.capacity = AC_EDF_STORAGE_QUEUE_CAPACITY;
    stats.available = true;
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
    copy_text(dir, sizeof(dir), path);
    char *last_slash = strrchr(dir, '/');
    if (!last_slash || last_slash == dir) return false;
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
    char field[8] = {};
    memset(field, ' ', sizeof(field));
    char text[16] = {};
    snprintf(text, sizeof(text), "%lu",
             static_cast<unsigned long>(state.record_count));
    const size_t len = strlen(text);
    if (len > sizeof(field)) return false;
    memcpy(field, text, len);
    if (!state.file.seek(236)) return false;
    const size_t written = state.file.write(
        reinterpret_cast<const uint8_t *>(field), sizeof(field));
    if (written != sizeof(field)) return false;
    state.file.flush();
    state.file.seek(state.file.size());
    return true;
}

bool write_header(OpenFile &state, const JobSlot &job) {
    const EdfFileSchema &schema = edf_numeric_schema(job.kind);
    const size_t header_size = edf_header_size(schema);
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
    const bool rendered = edf_render_header(schema, info, header,
                                            header_size, written);
    bool ok = false;
    if (rendered && written == header_size) {
        ok = state.file.write(header, header_size) == header_size;
    }
    Memory::free(header);
    return ok;
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
    state.path[0] = 0;
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
    (void)Storage::remove(job.path);
    state.file = Storage::open(job.path, "w+");
    if (!state.file) {
        stats.open_errors++;
        set_error("open_failed");
        return false;
    }
    state.kind = job.kind;
    state.record_count = job.record_count;
    copy_text(state.path, sizeof(state.path), job.path);
    if (!write_header(state, job)) {
        state.file.close();
        stats.write_errors++;
        set_error("header_write_failed");
        return false;
    }
    state.file.flush();
    state.open = true;
    copy_text(stats.last_path, sizeof(stats.last_path), job.path);
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
    copy_text(stats.last_path, sizeof(stats.last_path), state.path);
    stats.last_error[0] = 0;
    return true;
}

bool process_close(const JobSlot &job) {
    Storage::Guard guard;
    OpenFile &state = open_files[file_index(job.kind)];
    close_file(state);
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
            unlock_queue();
        }
        if (have_job) {
            process_job(job);
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

}  // namespace

void begin() {
    if (stats.initialized) return;
    stats.initialized = true;
    if (!queue_lock) queue_lock = xSemaphoreCreateMutex();
    if (!queue_lock || !allocate_slots()) {
        stats.available = false;
        return;
    }
    xTaskCreatePinnedToCore(task_entry, "ac_edf_storage",
                            AC_EDF_STORAGE_TASK_STACK, nullptr,
                            AC_EDF_STORAGE_TASK_PRIO, &task,
                            AC_EDF_STORAGE_TASK_CORE);
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
    JobSlot job;
    job.type = JobType::Open;
    job.kind = schema.kind;
    job.record_count = info.record_count;
    copy_text(job.path, sizeof(job.path), path);
    copy_text(job.patient_id, sizeof(job.patient_id), info.patient_id);
    copy_text(job.recording_id, sizeof(job.recording_id), info.recording_id);
    copy_text(job.start_date, sizeof(job.start_date), info.start_date);
    copy_text(job.start_time, sizeof(job.start_time), info.start_time);
    return enqueue(job);
}

bool enqueue_numeric_record(const EdfFileSchema &schema,
                            const EdfCompletedRecordView &record) {
    if (edf_record_size(schema) > AC_EDF_STORAGE_SLOT_BYTES) return false;
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
    job.type = JobType::Record;
    job.kind = schema.kind;
    size_t written = 0;
    const bool ok = edf_render_numeric_record(schema, record, job.bytes,
                                              AC_EDF_STORAGE_SLOT_BYTES,
                                              written);
    if (!ok) {
        unlock_queue();
        stats.render_errors++;
        set_error("render_failed");
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

bool enqueue_close_numeric(EdfFileKind kind) {
    JobSlot job;
    job.type = JobType::Close;
    job.kind = kind;
    return enqueue(job);
}

EdfStorageWorkerStatus status() {
    if (!stats.initialized) begin();
    EdfStorageWorkerStatus out = stats;
    if (lock_queue()) {
        out.queued = queued;
        unlock_queue();
    }
    return out;
}

}  // namespace EdfStorageWorker
}  // namespace aircannect
