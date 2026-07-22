#include "storage_service.h"

#include <algorithm>
#include <atomic>
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
#include "edf_str_file_layout.h"
#include "edf_str_record_merge.h"
#include "memory_manager.h"
#include "storage_archive_service.h"
#include "storage_browser_service.h"
#include "storage_delete_service.h"
#include "storage_file_log_sink.h"
#include "storage_manager.h"
#include "storage_path.h"
#include "string_util.h"

namespace aircannect {
namespace StorageService {
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

enum class MaintenanceOwner : uint8_t {
    None,
    Archive,
    Delete,
};

struct JobSlot {
    JobType type = JobType::Record;
    StoredFileKind kind = StoredFileKind::Brp;
    uint32_t request_id = 0;
    char path[sizeof(StorageServiceStatus::last_path)] = {};
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
    char path[sizeof(StorageServiceStatus::last_path)] = {};
};

struct OpenRequestResult {
    bool complete = false;
    bool success = false;
    bool open = false;
    bool resumed = false;
    uint32_t request_id = 0;
    uint32_t record_count = 0;
    char path[sizeof(StorageServiceStatus::last_path)] = {};
    char error[sizeof(StorageServiceStatus::last_error)] = {};
};

struct ReadJob {
    bool used = false;
    bool started = false;
    bool cancel_requested = false;
    bool abandon_requested = false;
    OperationTicket ticket;
    StorageReadMode mode = StorageReadMode::Range;
    StorageReadLane lane = StorageReadLane::Report;
    uint64_t sequence = 0;
    uint64_t offset = 0;
    size_t requested_length = 0;
    size_t target_length = 0;
    size_t bytes_read = 0;
    size_t tail_lines = 0;
    uint8_t *bytes = nullptr;
    char path[AC_STORAGE_PATH_MAX] = {};
};

struct PreparedReadSlot {
    bool used = false;
    StoragePreparedRead handle;
    uint8_t *bytes = nullptr;
};

struct ReadCompletionSlot {
    bool used = false;
    StorageReadCompletion completion;
};

void close_file(OpenFile &state);

StorageServiceStatus stats;
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

ReadJob read_jobs[AC_STORAGE_PREPARED_READ_CAPACITY];
PreparedReadSlot prepared_reads[AC_STORAGE_PREPARED_READ_CAPACITY];
ReadCompletionSlot read_completions[AC_STORAGE_PREPARED_READ_CAPACITY];
File active_read_file;
size_t active_read_index = SIZE_MAX;
size_t read_job_count = 0;
size_t prepared_read_count = 0;
uint32_t next_read_ticket_id = 0;
uint32_t next_prepared_read_id = 0;
uint64_t next_read_sequence = 0;
bool processing_read = false;

OperationSubmission submit_prepared_read(const StorageReadCommand &command);
bool cancel_prepared_read(OperationTicket ticket);
bool abandon_prepared_read(OperationTicket ticket);
bool take_read_completion(OperationTicket ticket,
                          StorageReadCompletion &completion);
size_t copy_prepared_read(StoragePreparedRead prepared,
                          size_t offset,
                          uint8_t *buffer,
                          size_t capacity);
void free_prepared_read(StoragePreparedRead prepared);

class ServiceReadPort final : public StorageReadPort {
public:
    OperationSubmission request_read(
        const StorageReadCommand &command) override {
        return submit_prepared_read(command);
    }

    bool cancel(OperationTicket ticket) override {
        return cancel_prepared_read(ticket);
    }

    bool abandon(OperationTicket ticket) override {
        return abandon_prepared_read(ticket);
    }

    bool take_completion(
        OperationTicket ticket,
        StorageReadCompletion &completion) override {
        return take_read_completion(ticket, completion);
    }

    size_t read_prepared(StoragePreparedRead prepared,
                         size_t offset,
                         uint8_t *buffer,
                         size_t capacity) const override {
        return copy_prepared_read(prepared, offset, buffer, capacity);
    }

    void release_prepared(StoragePreparedRead prepared) override {
        free_prepared_read(prepared);
    }
};

ServiceReadPort service_read_port;
std::atomic<MaintenanceOwner> maintenance_owner{MaintenanceOwner::None};
StorageBrowserService browser_service;
StorageArchiveService archive_service;
StorageDeleteService delete_service;
StorageFileLogSink file_log_sink;
size_t file_log_burst = 0;
bool browser_turn = true;

bool claim_maintenance(MaintenanceOwner owner) {
    MaintenanceOwner expected = MaintenanceOwner::None;
    return maintenance_owner.compare_exchange_strong(expected, owner);
}

void release_maintenance(MaintenanceOwner owner) {
    MaintenanceOwner expected = owner;
    (void)maintenance_owner.compare_exchange_strong(
        expected, MaintenanceOwner::None);
}

bool claim_archive_maintenance() {
    return claim_maintenance(MaintenanceOwner::Archive);
}

void release_archive_maintenance() {
    release_maintenance(MaintenanceOwner::Archive);
}

bool claim_delete_maintenance() {
    return claim_maintenance(MaintenanceOwner::Delete);
}

void release_delete_maintenance() {
    release_maintenance(MaintenanceOwner::Delete);
}

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

bool str_backup_artifact_name(const char *name) {
    if (!name || strncmp(name, "STR-", 4) != 0) return false;
    const size_t len = strlen(name);
    return len > 8 && strcmp(name + len - 4, ".bak") == 0;
}

void cleanup_str_backup_artifacts() {
    if (!Storage::mounted()) return;
    uint32_t removed = 0;
    Storage::Guard guard;
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
            str_backup_artifact_name(name) &&
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
    return len > 1 && len < sizeof(StorageServiceStatus::last_path);
}

bool lock_queue(uint32_t timeout_ms = 10) {
    if (!queue_lock) return false;
    return xSemaphoreTake(queue_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlock_queue() {
    if (queue_lock) xSemaphoreGive(queue_lock);
}

void wake_service_task() {
    if (task) xTaskNotifyGive(task);
}

uint8_t read_lane_priority(StorageReadLane lane) {
    switch (lane) {
        case StorageReadLane::Foreground: return 0;
        case StorageReadLane::Report: return 1;
        case StorageReadLane::Export: return 2;
        case StorageReadLane::Maintenance:
        default:
            return 3;
    }
}

void refresh_read_status_locked() {
    stats.read_capacity = AC_STORAGE_PREPARED_READ_CAPACITY;
    stats.read_queued = read_job_count;
    stats.prepared_reads = prepared_read_count;
}

ReadJob *find_read_job_locked(OperationTicket ticket) {
    for (ReadJob &job : read_jobs) {
        if (job.used && job.ticket == ticket) return &job;
    }
    return nullptr;
}

PreparedReadSlot *find_prepared_read_locked(StoragePreparedRead prepared) {
    if (!prepared.valid()) return nullptr;
    for (PreparedReadSlot &slot : prepared_reads) {
        if (slot.used && slot.handle.id == prepared.id) return &slot;
    }
    return nullptr;
}

size_t read_completion_count_locked() {
    size_t count = 0;
    for (const ReadCompletionSlot &slot : read_completions) {
        if (slot.used) count++;
    }
    return count;
}

ReadCompletionSlot *find_read_completion_locked(OperationTicket ticket) {
    if (!ticket.valid()) return nullptr;
    for (ReadCompletionSlot &slot : read_completions) {
        if (slot.used && slot.completion.ticket == ticket) return &slot;
    }
    return nullptr;
}

ReadCompletionSlot *find_free_read_completion_locked() {
    for (ReadCompletionSlot &slot : read_completions) {
        if (!slot.used) return &slot;
    }
    return nullptr;
}

size_t select_read_job_locked() {
    size_t selected = SIZE_MAX;
    uint8_t selected_priority = UINT8_MAX;
    uint64_t selected_sequence = UINT64_MAX;

    for (size_t i = 0; i < AC_STORAGE_PREPARED_READ_CAPACITY; ++i) {
        const ReadJob &job = read_jobs[i];
        if (!job.used) continue;

        if (job.cancel_requested) return i;

        const uint8_t priority = read_lane_priority(job.lane);
        if (selected == SIZE_MAX || priority < selected_priority ||
            (priority == selected_priority &&
             job.sequence < selected_sequence)) {
            selected = i;
            selected_priority = priority;
            selected_sequence = job.sequence;
        }
    }
    return selected;
}

OperationTicket next_read_ticket_locked(uint32_t generation) {
    next_read_ticket_id++;
    if (next_read_ticket_id == 0) next_read_ticket_id++;
    return {next_read_ticket_id, generation};
}

StoragePreparedRead next_prepared_handle_locked(size_t length) {
    next_prepared_read_id++;
    if (next_prepared_read_id == 0) next_prepared_read_id++;
    return {next_prepared_read_id, length};
}

OperationSubmission submit_prepared_read(const StorageReadCommand &command) {
    if (!command.valid() || command.path.size() >= AC_STORAGE_PATH_MAX ||
        command.offset > UINT32_MAX ||
        command.length > AC_STORAGE_PREPARED_READ_MAX_BYTES) {
        return OperationSubmission::rejected();
    }

    if (!stats.initialized) begin();
    if (!stats.available || !lock_queue()) return OperationSubmission::busy();

    const size_t completion_count = read_completion_count_locked();
    if (read_job_count + completion_count >=
            AC_STORAGE_PREPARED_READ_CAPACITY ||
        read_job_count + prepared_read_count >=
            AC_STORAGE_PREPARED_READ_CAPACITY) {
        unlock_queue();
        return OperationSubmission::busy();
    }

    ReadJob *free_job = nullptr;
    for (ReadJob &job : read_jobs) {
        if (!job.used) {
            free_job = &job;
            break;
        }
    }
    if (!free_job) {
        unlock_queue();
        return OperationSubmission::busy();
    }

    const OperationTicket ticket =
        next_read_ticket_locked(command.generation);
    *free_job = {};
    free_job->used = true;
    free_job->ticket = ticket;
    free_job->mode = command.mode;
    free_job->lane = command.lane;
    free_job->sequence = ++next_read_sequence;
    free_job->offset = command.offset;
    free_job->requested_length = command.length;
    free_job->tail_lines = command.tail_lines;
    copy_cstr(free_job->path, sizeof(free_job->path), command.path.c_str());
    read_job_count++;
    refresh_read_status_locked();
    unlock_queue();

    wake_service_task();
    return OperationSubmission::accepted(ticket);
}

bool cancel_prepared_read(OperationTicket ticket) {
    if (!ticket.valid() || !lock_queue()) return false;

    ReadJob *job = find_read_job_locked(ticket);
    if (!job) {
        unlock_queue();
        return false;
    }
    job->cancel_requested = true;
    unlock_queue();

    wake_service_task();
    return true;
}

bool abandon_prepared_read(OperationTicket ticket) {
    if (!ticket.valid() || !lock_queue()) return false;

    ReadJob *job = find_read_job_locked(ticket);
    if (job) {
        job->cancel_requested = true;
        job->abandon_requested = true;
        unlock_queue();
        wake_service_task();
        return true;
    }

    ReadCompletionSlot *completion =
        find_read_completion_locked(ticket);
    if (!completion) {
        unlock_queue();
        return false;
    }

    uint8_t *bytes = nullptr;
    PreparedReadSlot *prepared =
        find_prepared_read_locked(completion->completion.prepared);
    if (prepared) {
        bytes = prepared->bytes;
        *prepared = {};
        if (prepared_read_count > 0) prepared_read_count--;
    }
    *completion = {};
    refresh_read_status_locked();
    unlock_queue();

    Memory::free(bytes);
    return true;
}

bool take_read_completion(OperationTicket ticket,
                          StorageReadCompletion &completion) {
    if (!ticket.valid() || !lock_queue()) return false;

    ReadCompletionSlot *slot = find_read_completion_locked(ticket);
    if (!slot) {
        unlock_queue();
        return false;
    }

    completion = slot->completion;
    *slot = {};
    unlock_queue();
    return true;
}

size_t copy_prepared_read(StoragePreparedRead prepared,
                          size_t offset,
                          uint8_t *buffer,
                          size_t capacity) {
    if (!buffer || capacity == 0 || !lock_queue()) return 0;

    const PreparedReadSlot *slot = find_prepared_read_locked(prepared);
    if (!slot || offset >= slot->handle.length) {
        unlock_queue();
        return 0;
    }

    const size_t length =
        std::min(capacity, slot->handle.length - offset);
    memcpy(buffer, slot->bytes + offset, length);
    unlock_queue();
    return length;
}

void free_prepared_read(StoragePreparedRead prepared) {
    if (!prepared.valid() || !lock_queue()) return;

    PreparedReadSlot *slot = find_prepared_read_locked(prepared);
    if (!slot) {
        unlock_queue();
        return;
    }

    uint8_t *bytes = slot->bytes;
    *slot = {};
    if (prepared_read_count > 0) prepared_read_count--;
    refresh_read_status_locked();
    unlock_queue();

    Memory::free(bytes);
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
    stats.edf_capacity = AC_EDF_STORAGE_QUEUE_CAPACITY;
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
    stats.edf_queued = queued;
    return true;
}

bool ensure_parent_dirs(const char *path) {
    if (!valid_path(path)) return false;
    char dir[sizeof(StorageServiceStatus::last_path)] = {};
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
    if (!header) {
        log_alloc_failed("str_header", header_size);
        return false;
    }

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
        if (!actual_header) log_alloc_failed("str_actual_header", header_size);
        if (!expected_header) {
            log_alloc_failed("str_expected_header", header_size);
        }
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
            file.close();
            if (!Storage::remove(job.path)) {
                stats.write_errors++;
                set_error("str_replace_failed");
                return false;
            }
            Log::logf(CAT_EDF, LOG_WARN,
                      "discarded incompatible STR root file path=%s\n",
                      job.path);

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

void close_active_read_file() {
    if (active_read_file) {
        Storage::Guard guard;
        active_read_file.close();
    }
    active_read_index = SIZE_MAX;
}

void finish_read_job(size_t index,
                     OperationOutcome outcome,
                     const char *error = nullptr) {
    if (index >= AC_STORAGE_PREPARED_READ_CAPACITY) return;
    if (active_read_index == index) close_active_read_file();

    uint8_t *bytes_to_free = nullptr;
    if (!lock_queue(50)) {
        set_error("read_finish_lock_failed");
        return;
    }

    ReadJob &job = read_jobs[index];
    if (!job.used) {
        unlock_queue();
        return;
    }

    StorageReadCompletion completion;
    completion.ticket = job.ticket;
    completion.outcome = outcome;
    const bool abandoned = job.abandon_requested;

    PreparedReadSlot *prepared_slot = nullptr;
    if (!abandoned &&
        outcome.disposition == OperationDisposition::Succeeded) {
        for (PreparedReadSlot &slot : prepared_reads) {
            if (!slot.used) {
                prepared_slot = &slot;
                break;
            }
        }
        if (!prepared_slot) {
            completion.outcome = OperationOutcome::failed();
            error = "prepared_read_slots_full";
        } else {
            prepared_slot->used = true;
            prepared_slot->handle =
                next_prepared_handle_locked(job.bytes_read);
            prepared_slot->bytes = job.bytes;
            completion.prepared = prepared_slot->handle;
            job.bytes = nullptr;
            prepared_read_count++;
        }
    }

    bytes_to_free = job.bytes;
    job.bytes = nullptr;
    job = {};
    if (read_job_count > 0) read_job_count--;

    ReadCompletionSlot *completion_slot = nullptr;
    if (!abandoned) {
        completion_slot = find_free_read_completion_locked();
    }
    if (!abandoned && !completion_slot) {
        if (prepared_slot) {
            bytes_to_free = prepared_slot->bytes;
            *prepared_slot = {};
            if (prepared_read_count > 0) prepared_read_count--;
        }
        set_error("read_completion_queue_full");
    } else if (completion_slot) {
        completion_slot->used = true;
        completion_slot->completion = completion;
    }

    if (abandoned) {
        stats.read_cancellations++;
    } else if (completion_slot &&
        completion.outcome.disposition == OperationDisposition::Succeeded) {
        stats.read_jobs++;
        stats.bytes_read += completion.prepared.length;
        stats.last_error[0] = 0;
    } else if (completion_slot &&
               completion.outcome.disposition ==
                   OperationDisposition::Cancelled) {
        stats.read_cancellations++;
    } else if (completion_slot) {
        stats.read_errors++;
        set_error(error ? error : "read_failed");
    }

    processing_read = read_job_count > 0;
    refresh_read_status_locked();
    unlock_queue();

    Memory::free(bytes_to_free);
}

bool open_read_job(size_t index, const char *&error) {
    if (index >= AC_STORAGE_PREPARED_READ_CAPACITY) {
        error = "read_index_invalid";
        return false;
    }

    ReadJob &job = read_jobs[index];
    if (!job.used) {
        error = "read_job_missing";
        return false;
    }

    if (active_read_index != index) {
        close_active_read_file();

        Storage::Guard guard;
        active_read_file = Storage::open(job.path, "r");
        if (!active_read_file || active_read_file.isDirectory()) {
            if (active_read_file) active_read_file.close();
            error = "read_open_failed";
            return false;
        }
        active_read_index = index;
    }

    if (!job.started) {
        size_t file_size = 0;
        {
            Storage::Guard guard;
            file_size = active_read_file.size();
        }

        if (job.mode == StorageReadMode::TailLines) {
            job.target_length = std::min(job.requested_length, file_size);
            job.offset = file_size - job.target_length;
        } else {
            if (job.offset >= file_size) {
                error = "read_offset_out_of_range";
                return false;
            }

            const size_t available =
                file_size - static_cast<size_t>(job.offset);
            job.target_length = std::min(job.requested_length, available);
        }

        if (job.target_length > 0) {
            job.bytes = static_cast<uint8_t *>(
                Memory::alloc_large(job.target_length, false));
            if (!job.bytes) {
                error = "read_allocation_failed";
                return false;
            }
        }
        job.started = true;
    }

    if (job.target_length == 0) return true;

    Storage::Guard guard;
    const uint64_t position = job.offset + job.bytes_read;
    if (position > UINT32_MAX ||
        !active_read_file.seek(static_cast<uint32_t>(position))) {
        error = "read_seek_failed";
        return false;
    }
    return true;
}

void trim_read_to_tail_lines(ReadJob &job) {
    if (job.mode != StorageReadMode::TailLines || !job.bytes ||
        job.bytes_read == 0) {
        return;
    }

    size_t suffix_lines = job.bytes[job.bytes_read - 1] == '\n' ? 0 : 1;
    size_t start = 0;
    for (size_t i = job.bytes_read; i > 0; --i) {
        if (job.bytes[i - 1] != '\n') continue;

        if (suffix_lines >= job.tail_lines) {
            start = i;
            break;
        }
        suffix_lines++;
    }

    if (start == 0 && job.offset > 0) {
        while (start < job.bytes_read && job.bytes[start] != '\n') start++;
        if (start < job.bytes_read) start++;
    }
    if (start == 0) return;

    job.bytes_read -= start;
    memmove(job.bytes, job.bytes + start, job.bytes_read);
    job.target_length = job.bytes_read;
}

bool process_read_step() {
    if (!lock_queue(50)) return false;
    const size_t index = select_read_job_locked();
    if (index == SIZE_MAX) {
        processing_read = false;
        unlock_queue();
        close_active_read_file();
        return false;
    }
    processing_read = true;
    const bool cancelled = read_jobs[index].cancel_requested;
    unlock_queue();

    if (cancelled) {
        finish_read_job(index, OperationOutcome::cancelled());
        return true;
    }

    ReadJob &job = read_jobs[index];
    const bool file_log_tail =
        job.mode == StorageReadMode::TailLines &&
        strcmp(job.path, AC_FILE_LOG_PATH) == 0;
    if (!job.started && file_log_tail &&
        !file_log_sink.prepare_tail_read()) {
        return false;
    }

    const char *error = nullptr;
    if (!Storage::mounted()) {
        finish_read_job(index,
                        OperationOutcome::failed(),
                        "storage_not_mounted");
        return true;
    }
    if (!open_read_job(index, error)) {
        finish_read_job(index, OperationOutcome::failed(), error);
        return true;
    }

    if (job.target_length == 0) {
        finish_read_job(index, OperationOutcome::succeeded());
        return true;
    }

    const size_t remaining = job.target_length - job.bytes_read;
    const size_t requested = std::min(remaining, AC_STORAGE_READ_STEP_BYTES);
    size_t received = 0;
    {
        Storage::Guard guard;
        const int read = active_read_file.read(job.bytes + job.bytes_read,
                                               requested);
        if (read > 0) received = static_cast<size_t>(read);
    }
    if (received == 0) {
        finish_read_job(index,
                        OperationOutcome::failed(),
                        "read_short_file");
        return true;
    }

    job.bytes_read += received;
    stats.last_activity_ms = millis();
    copy_cstr(stats.last_path, sizeof(stats.last_path), job.path);
    if (job.bytes_read == job.target_length) {
        trim_read_to_tail_lines(job);
        finish_read_job(index, OperationOutcome::succeeded());
    }
    return true;
}

bool file_log_tail_read_active() {
    if (active_read_index >= AC_STORAGE_PREPARED_READ_CAPACITY) return false;

    const ReadJob &job = read_jobs[active_read_index];
    return job.used && job.mode == StorageReadMode::TailLines &&
           strcmp(job.path, AC_FILE_LOG_PATH) == 0;
}

bool process_browser_step() {
    return browser_service.step() == StorageBrowserStep::Working;
}

bool process_foreground_step() {
    if (browser_turn) {
        if (process_browser_step()) {
            browser_turn = false;
            return true;
        }
        if (process_read_step()) {
            browser_turn = true;
            return true;
        }
        return false;
    }

    if (process_read_step()) {
        browser_turn = true;
        return true;
    }
    if (process_browser_step()) {
        browser_turn = false;
        return true;
    }
    return false;
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
    cleanup_str_backup_artifacts();
    stats.task_started = true;

    for (;;) {
        bool have_job = false;
        size_t slot_index = SIZE_MAX;
        if (lock_queue(50)) {
            if (slots && queued > 0 && !processing_job) {
                slot_index = head;
                head = (head + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
                queued--;
                stats.edf_queued = queued;
                processing_job = true;
                have_job = true;
            }
            unlock_queue();
        }

        bool did_work = false;
        if (have_job) {
            process_job(slots[slot_index]);
            if (lock_queue(50)) {
                clear_slot(slots[slot_index]);
                processing_job = false;
                unlock_queue();
            } else {
                processing_job = false;
            }
            did_work = true;
            file_log_burst = 0;
        } else {
            const bool foreground_due =
                file_log_burst >= AC_FILE_LOG_DRAIN_BUDGET;
            const bool tail_read_active = file_log_tail_read_active();
            if (!foreground_due && !tail_read_active &&
                file_log_sink.step()) {
                did_work = true;
                file_log_burst++;
            } else if (process_foreground_step()) {
                did_work = true;
                file_log_burst = 0;
            } else if (archive_service.step()) {
                did_work = true;
            } else if (delete_service.step()) {
                did_work = true;
            } else if (foreground_due && !tail_read_active &&
                       file_log_sink.step()) {
                did_work = true;
                file_log_burst++;
            }
        }

        if (did_work) {
            vTaskDelay(pdMS_TO_TICKS(AC_STORAGE_SERVICE_WORK_TICK_MS));
        } else {
            ulTaskNotifyTake(pdTRUE,
                             pdMS_TO_TICKS(
                                 AC_STORAGE_SERVICE_IDLE_TICK_MS));
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
    wake_service_task();
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
    stats.edf_queued = queued;
    unlock_queue();

    stats.bytes_enqueued += written;
    stats.last_activity_ms = millis();
    wake_service_task();
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

    (void)file_log_sink.begin();
    (void)browser_service.begin(wake_service_task);
    (void)archive_service.begin(wake_service_task,
                                claim_archive_maintenance,
                                release_archive_maintenance);
    (void)delete_service.begin(wake_service_task,
                               claim_delete_maintenance,
                               release_delete_maintenance);

    if (!task) {
        const BaseType_t created =
            xTaskCreatePinnedToCore(task_entry, "ac_storage",
                                    AC_STORAGE_SERVICE_TASK_STACK, nullptr,
                                    AC_STORAGE_SERVICE_TASK_PRIO, &task,
                                    AC_STORAGE_SERVICE_TASK_CORE);
        if (created != pdPASS || !task) {
            stats.available = false;
            browser_service.set_task_available(false);
            archive_service.set_task_available(false);
            delete_service.set_task_available(false);
            set_error("task_create_failed");
            Log::logf(CAT_EDF, LOG_ERROR,
                      "storage worker task create failed\n");
            return;
        }
    }
    stats.initialized = true;
    stats.available = true;
    stats.read_capacity = AC_STORAGE_PREPARED_READ_CAPACITY;
    browser_service.set_task_available(true);
    archive_service.set_task_available(true);
    delete_service.set_task_available(true);

    Log::logf(CAT_STORAGE, LOG_DEBUG,
              "service ready edf_q=%u read_q=%u slot=%u psram=%s\n",
              static_cast<unsigned>(AC_EDF_STORAGE_QUEUE_CAPACITY),
              static_cast<unsigned>(AC_STORAGE_PREPARED_READ_CAPACITY),
              static_cast<unsigned>(AC_EDF_STORAGE_SLOT_BYTES),
              stats.using_psram ? "yes" : "no");
}

bool enqueue_edf_open_numeric(const char *path,
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

bool enqueue_edf_open_annotation(const char *path,
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

bool enqueue_edf_numeric_record(const EdfFileSchema &schema,
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
    stats.edf_queued = queued;
    unlock_queue();

    stats.bytes_enqueued += queued_len;
    stats.last_activity_ms = millis();
    wake_service_task();
    return true;
}

bool enqueue_edf_annotation_record(EdfAnnotationKind kind,
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

bool enqueue_edf_str_record(const char *path,
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

bool enqueue_edf_identification_files(const std::string &json) {
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

bool enqueue_edf_close_numeric(EdfFileKind kind) {
    JobSlot job;
    job.type = JobType::Close;
    job.kind = stored_kind(kind);
    return enqueue(job);
}

bool enqueue_edf_close_annotation(EdfAnnotationKind kind) {
    JobSlot job;
    job.type = JobType::Close;
    job.kind = stored_kind(kind);
    return enqueue(job);
}

StorageReadPort &read_port() {
    return service_read_port;
}

StorageBrowserPort &browser_port() {
    return browser_service;
}

StorageArchivePort &archive_port() {
    return archive_service;
}

StorageDeletePort &delete_port() {
    return delete_service;
}

bool configure_file_log(bool enabled) {
    if (!file_log_sink.begin()) return false;
    file_log_sink.set_enabled(enabled);
    if (stats.initialized) wake_service_task();
    return true;
}

bool enqueue_file_log_line(const char *line, size_t length) {
    if (!file_log_sink.begin()) return false;
    const bool accepted = file_log_sink.enqueue(line, length);
    if (accepted && stats.initialized) wake_service_task();
    return accepted;
}

StorageFileLogStatus file_log_status() {
    return file_log_sink.status();
}

void publish_activity(const ActivitySnapshot &activity) {
    file_log_sink.set_rotation_allowed(!activity.therapy_active &&
                                       !activity.ota_install_active);
    const bool maintenance_paused =
        activity.therapy_active || activity.realtime_stream_active ||
        activity.foreground_report_demand || activity.ota_install_active ||
        activity.export_active;
    archive_service.set_paused(maintenance_paused);
    delete_service.set_paused(maintenance_paused);
    wake_service_task();
}

StorageServiceStatus status() {
    StorageServiceStatus out = stats;
    if (lock_queue()) {
        refresh_read_status_locked();
        out = stats;
        out.edf_queued = queued;
        out.busy = processing_job || processing_read;
        unlock_queue();
    } else {
        out.busy = processing_job || processing_read;
    }
    out.maintenance_active =
        maintenance_owner.load() != MaintenanceOwner::None;
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

bool edf_open_result(const EdfStorageOpenHandle &handle,
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

}  // namespace StorageService
}  // namespace aircannect
