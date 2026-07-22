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
#include "edf_str_storage_writer.h"
#include "memory_manager.h"
#include "storage_archive_service.h"
#include "storage_atomic_write_service.h"
#include "storage_browser_service.h"
#include "storage_delete_service.h"
#include "storage_file_log_sink.h"
#include "storage_internal.h"
#include "storage_path.h"
#include "storage_path_service.h"
#include "storage_scan_service.h"
#include "storage_stream_service.h"
#include "storage_upload_service.h"
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
    Scan,
    Archive,
    Delete,
    Upload,
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
    bool file_log_fence_captured = false;
    bool file_log_snapshot_active = false;
    uint32_t file_log_fence_sequence = 0;
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
PreparedByteRead copy_prepared_read(StoragePreparedRead prepared,
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

    PreparedByteRead read_prepared(StoragePreparedRead prepared,
                                   size_t offset,
                                   uint8_t *buffer,
                                   size_t capacity) const override {
        return copy_prepared_read(prepared, offset, buffer, capacity);
    }

    void release_prepared(StoragePreparedRead prepared) override {
        free_prepared_read(prepared);
    }
};

class ServiceStatusPort final : public StorageStatusPort {
public:
    bool mounted() const override { return Storage::mounted(); }
    StorageWorkloadSnapshot workload_snapshot() const override {
        return StorageService::workload_snapshot();
    }
};

ServiceReadPort service_read_port;
ServiceStatusPort service_status_port;
std::atomic<MaintenanceOwner> maintenance_owner{MaintenanceOwner::None};
StorageBrowserService browser_service;
StorageArchiveService archive_service;
StorageDeleteService delete_service;
StoragePathService path_service;
StorageAtomicWriteService atomic_write_service;
StorageScanService scan_service;
StorageStreamService stream_service;
StorageUploadService upload_service;
StorageFileLogSink file_log_sink;
size_t file_log_burst = 0;
size_t foreground_turn = 0;
std::atomic<bool> mount_retry_requested{false};
std::atomic<bool> capacity_update_allowed{true};
bool storage_resources_ready = false;
uint32_t storage_resource_retry_at_ms = 0;
uint8_t storage_resource_retry_attempt = 0;
uint32_t mount_retry_at_ms = 0;
uint8_t mount_retry_attempt = 0;

static constexpr size_t AC_STORAGE_DIAGNOSTIC_PAYLOAD_CAPACITY = 512;
static constexpr uint32_t STORAGE_RESOURCE_RETRY_MIN_MS = 1000;
static constexpr uint32_t STORAGE_RESOURCE_RETRY_MAX_MS = 30000;
static constexpr uint32_t STORAGE_MOUNT_RETRY_MIN_MS = 5000;
static constexpr uint32_t STORAGE_MOUNT_RETRY_MAX_MS = 60000;
StorageDiagnosticStatus diagnostic;
uint8_t *diagnostic_payload = nullptr;
size_t diagnostic_payload_length = 0;

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

bool claim_scan_maintenance() {
    return claim_maintenance(MaintenanceOwner::Scan);
}

void release_scan_maintenance() {
    release_maintenance(MaintenanceOwner::Scan);
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

bool claim_upload_maintenance() {
    return claim_maintenance(MaintenanceOwner::Upload);
}

void release_upload_maintenance() {
    release_maintenance(MaintenanceOwner::Upload);
}

constexpr size_t max_size(size_t a, size_t b) {
    return a > b ? a : b;
}

bool deadline_due(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_ms == 0 ||
           static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

uint32_t retry_delay(uint8_t attempt,
                     uint32_t minimum_ms,
                     uint32_t maximum_ms) {
    uint32_t delay_ms = minimum_ms;
    for (uint8_t i = 0; i < attempt && delay_ms < maximum_ms; ++i) {
        delay_ms = std::min(delay_ms * 2u, maximum_ms);
    }
    return delay_ms;
}

void advance_retry(uint8_t &attempt) {
    if (attempt < 8) ++attempt;
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

PreparedByteRead copy_prepared_read(StoragePreparedRead prepared,
                                    size_t offset,
                                    uint8_t *buffer,
                                    size_t capacity) {
    PreparedByteRead result;
    if (!prepared.valid() || !buffer || capacity == 0) return result;

    if (!lock_queue()) {
        result.state = PreparedByteReadState::Retry;
        return result;
    }

    const PreparedReadSlot *slot = find_prepared_read_locked(prepared);
    if (!slot || offset >= slot->handle.length) {
        unlock_queue();
        return result;
    }

    result.state = PreparedByteReadState::Data;
    result.bytes = std::min(capacity, slot->handle.length - offset);
    memcpy(buffer, slot->bytes + offset, result.bytes);
    unlock_queue();
    return result;
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

bool slot_storage_available() {
    return slots && slot_bytes && slot_numeric_values &&
           slot_numeric_present && slot_numeric_valid;
}

void free_slot_storage(JobSlot *candidate_slots,
                       uint8_t *candidate_bytes,
                       float *candidate_numeric_values,
                       uint8_t *candidate_numeric_present,
                       uint8_t *candidate_numeric_valid) {
    Memory::free(candidate_slots);
    Memory::free(candidate_bytes);
    Memory::free(candidate_numeric_values);
    Memory::free(candidate_numeric_present);
    Memory::free(candidate_numeric_valid);
}

void bind_slot_storage(JobSlot *candidate_slots,
                       uint8_t *candidate_bytes,
                       float *candidate_numeric_values,
                       uint8_t *candidate_numeric_present,
                       uint8_t *candidate_numeric_valid,
                       size_t index) {
    JobSlot &slot = candidate_slots[index];
    slot.bytes = candidate_bytes + index * AC_EDF_STORAGE_SLOT_BYTES;
    slot.numeric_values =
        candidate_numeric_values +
        index * AC_EDF_STORAGE_NUMERIC_VALUE_MAX;
    slot.numeric_present =
        candidate_numeric_present +
        index * AC_EDF_STORAGE_NUMERIC_BIT_BYTES;
    slot.numeric_valid =
        candidate_numeric_valid +
        index * AC_EDF_STORAGE_NUMERIC_BIT_BYTES;
}

bool allocate_slots() {
    if (slot_storage_available()) return true;

    JobSlot *candidate_slots = static_cast<JobSlot *>(Memory::calloc_large(
        AC_EDF_STORAGE_QUEUE_CAPACITY, sizeof(JobSlot), false));
    uint8_t *candidate_bytes = static_cast<uint8_t *>(Memory::alloc_large(
        AC_EDF_STORAGE_SLOT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY, false));
    float *candidate_numeric_values = static_cast<float *>(
        Memory::alloc_large(AC_EDF_STORAGE_NUMERIC_VALUE_MAX *
                                AC_EDF_STORAGE_QUEUE_CAPACITY * sizeof(float),
                            false));
    uint8_t *candidate_numeric_present = static_cast<uint8_t *>(
        Memory::alloc_large(AC_EDF_STORAGE_NUMERIC_BIT_BYTES *
                                AC_EDF_STORAGE_QUEUE_CAPACITY,
                            false));
    uint8_t *candidate_numeric_valid = static_cast<uint8_t *>(
        Memory::alloc_large(
            AC_EDF_STORAGE_NUMERIC_BIT_BYTES *
                AC_EDF_STORAGE_QUEUE_CAPACITY,
            false));
    const bool complete = candidate_slots && candidate_bytes &&
                          candidate_numeric_values &&
                          candidate_numeric_present &&
                          candidate_numeric_valid;
    if (!complete) {
        if (!candidate_slots) {
            log_alloc_failed("queue_slots",
                             AC_EDF_STORAGE_QUEUE_CAPACITY * sizeof(JobSlot));
        }
        if (!candidate_bytes) {
            log_alloc_failed(
                "queue_payloads",
                AC_EDF_STORAGE_SLOT_BYTES * AC_EDF_STORAGE_QUEUE_CAPACITY);
        }
        if (!candidate_numeric_values) {
            log_alloc_failed("queue_numeric_values",
                             AC_EDF_STORAGE_NUMERIC_VALUE_MAX *
                                 AC_EDF_STORAGE_QUEUE_CAPACITY *
                                 sizeof(float));
        }
        if (!candidate_numeric_present) {
            log_alloc_failed("queue_numeric_present",
                             AC_EDF_STORAGE_NUMERIC_BIT_BYTES *
                                 AC_EDF_STORAGE_QUEUE_CAPACITY);
        }
        if (!candidate_numeric_valid) {
            log_alloc_failed("queue_numeric_valid",
                             AC_EDF_STORAGE_NUMERIC_BIT_BYTES *
                                 AC_EDF_STORAGE_QUEUE_CAPACITY);
        }
        free_slot_storage(candidate_slots,
                          candidate_bytes,
                          candidate_numeric_values,
                          candidate_numeric_present,
                          candidate_numeric_valid);
        set_error("edf_queue_allocation_failed");
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STORAGE_QUEUE_CAPACITY; ++i) {
        bind_slot_storage(candidate_slots,
                          candidate_bytes,
                          candidate_numeric_values,
                          candidate_numeric_present,
                          candidate_numeric_valid,
                          i);
    }

    if (!lock_queue(50)) {
        free_slot_storage(candidate_slots,
                          candidate_bytes,
                          candidate_numeric_values,
                          candidate_numeric_present,
                          candidate_numeric_valid);
        return false;
    }

    if (slot_storage_available()) {
        unlock_queue();
        free_slot_storage(candidate_slots,
                          candidate_bytes,
                          candidate_numeric_values,
                          candidate_numeric_present,
                          candidate_numeric_valid);
        return true;
    }

    slots = candidate_slots;
    slot_bytes = candidate_bytes;
    slot_numeric_values = candidate_numeric_values;
    slot_numeric_present = candidate_numeric_present;
    slot_numeric_valid = candidate_numeric_valid;
    stats.using_psram = Memory::psram_available();
    stats.edf_capacity = AC_EDF_STORAGE_QUEUE_CAPACITY;
    if (strcmp(stats.last_error, "edf_queue_allocation_failed") == 0) {
        stats.last_error[0] = '\0';
    }
    unlock_queue();
    return true;
}

bool allocate_diagnostic_payload() {
    if (diagnostic_payload) {
        diagnostic.available = true;
        return true;
    }

    diagnostic_payload = static_cast<uint8_t *>(
        Memory::alloc_large(AC_STORAGE_DIAGNOSTIC_PAYLOAD_CAPACITY, false));
    diagnostic.available = diagnostic_payload != nullptr;
    if (!diagnostic.available) {
        diagnostic.state = StorageDiagnosticState::Failed;
        copy_cstr(diagnostic.error, sizeof(diagnostic.error),
                  "allocation_failed");
        return false;
    }

    if (strcmp(diagnostic.error, "allocation_failed") == 0) {
        diagnostic.state = StorageDiagnosticState::Idle;
        diagnostic.error[0] = '\0';
    }
    return true;
}

bool initialize_storage_resources() {
    bool ready = allocate_slots();
    if (!allocate_diagnostic_payload()) ready = false;
#if AC_FILE_LOG_ENABLED
    if (!file_log_sink.begin(wake_service_task)) ready = false;
#endif
    if (!browser_service.begin(wake_service_task)) ready = false;
    if (!archive_service.begin(wake_service_task,
                               claim_archive_maintenance,
                               release_archive_maintenance)) {
        ready = false;
    }
    if (!delete_service.begin(wake_service_task,
                              claim_delete_maintenance,
                              release_delete_maintenance)) {
        ready = false;
    }
    if (!path_service.begin(wake_service_task)) ready = false;
    if (!atomic_write_service.begin(wake_service_task)) ready = false;
    if (!upload_service.begin(wake_service_task,
                              atomic_write_service,
                              claim_upload_maintenance,
                              release_upload_maintenance)) {
        ready = false;
    }
    if (!scan_service.begin(wake_service_task,
                            claim_scan_maintenance,
                            release_scan_maintenance)) {
        ready = false;
    }
    if (!stream_service.begin(wake_service_task)) ready = false;
    return ready;
}

void set_storage_task_available(bool available) {
    browser_service.set_task_available(available);
    archive_service.set_task_available(available);
    delete_service.set_task_available(available);
    path_service.set_task_available(available);
    atomic_write_service.set_task_available(available);
    upload_service.set_task_available(available);
    scan_service.set_task_available(available);
    stream_service.set_task_available(available);
}

bool process_storage_resource_recovery(uint32_t now_ms) {
    if (storage_resources_ready ||
        !deadline_due(now_ms, storage_resource_retry_at_ms)) {
        return false;
    }

    storage_resources_ready = initialize_storage_resources();
    if (storage_resources_ready) {
        storage_resource_retry_at_ms = 0;
        storage_resource_retry_attempt = 0;
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "storage service resources recovered\n");
        return true;
    }

    storage_resource_retry_at_ms =
        now_ms + retry_delay(storage_resource_retry_attempt,
                             STORAGE_RESOURCE_RETRY_MIN_MS,
                             STORAGE_RESOURCE_RETRY_MAX_MS);
    advance_retry(storage_resource_retry_attempt);
    return true;
}

bool process_mount_recovery(uint32_t now_ms) {
    const bool manual_retry = mount_retry_requested.exchange(false);
    const StorageStatus storage = Storage::status();
    if (storage.mounted) {
        mount_retry_at_ms = 0;
        mount_retry_attempt = 0;
        return false;
    }

    const bool automatic_retry =
        storage.configured && storage.state == StorageState::NotPresent;
    if (!manual_retry && !automatic_retry) {
        mount_retry_at_ms = 0;
        mount_retry_attempt = 0;
        return false;
    }

    if (!manual_retry && !deadline_due(now_ms, mount_retry_at_ms)) {
        return false;
    }

    if (Storage::retry_mount()) {
        mount_retry_at_ms = 0;
        mount_retry_attempt = 0;
        recover_str_storage_artifacts();
        return true;
    }

    mount_retry_at_ms =
        now_ms + retry_delay(mount_retry_attempt,
                             STORAGE_MOUNT_RETRY_MIN_MS,
                             STORAGE_MOUNT_RETRY_MAX_MS);
    advance_retry(mount_retry_attempt);
    return true;
}

bool push_slot(const JobSlot &job) {
    if (!slot_storage_available() ||
        queued >= AC_EDF_STORAGE_QUEUE_CAPACITY) {
        return false;
    }
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
    copy_cstr(completion.error, sizeof(completion.error), error ? error : "");
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
            copy_cstr(completion.error,
                      sizeof(completion.error),
                      error);
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
            file_size = active_read_file.size();
        }

        if (job.mode == StorageReadMode::TailLines) {
            job.target_length = std::min(job.requested_length, file_size);
            job.offset = file_size - job.target_length;
        } else {
            if (job.offset > file_size ||
                (job.offset == file_size && file_size != 0)) {
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
    if (!job.started && file_log_tail) {
        if (!job.file_log_fence_captured) {
            job.file_log_fence_sequence =
                file_log_sink.capture_tail_fence();
            job.file_log_fence_captured = true;
        }

        if (!job.file_log_snapshot_active) {
            if (!file_log_sink.prepare_tail_read(
                    job.file_log_fence_sequence)) {
                return false;
            }
            job.file_log_snapshot_active = true;
        }
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
    return job.used && job.file_log_snapshot_active &&
           job.mode == StorageReadMode::TailLines &&
           strcmp(job.path, AC_FILE_LOG_PATH) == 0;
}

bool process_browser_step() {
    return browser_service.step() == StorageBrowserStep::Working;
}

bool process_stream_step() {
    return stream_service.step();
}

bool process_foreground_step() {
    if (atomic_write_service.step(StorageAtomicWriteLane::Foreground)) {
        return true;
    }
    if (path_service.step()) return true;

    for (size_t attempt = 0; attempt < 4; ++attempt) {
        const size_t current = foreground_turn;
        foreground_turn = (foreground_turn + 1) % 4;

        bool worked = false;
        if (current == 0) {
            worked = process_browser_step();
        } else if (current == 1) {
            worked = process_read_step();
        } else if (current == 2) {
            worked = process_stream_step();
        } else {
            worked = upload_service.step();
        }
        if (worked) return true;
    }
    return false;
}

bool process_diagnostic_step() {
    if (!lock_queue(20)) return false;
    if (diagnostic.state != StorageDiagnosticState::Queued) {
        unlock_queue();
        return false;
    }

    diagnostic.state = StorageDiagnosticState::Writing;
    const size_t payload_length = diagnostic_payload_length;
    char path[AC_STORAGE_WRITE_PATH_MAX] = {};
    copy_cstr(path, sizeof(path), diagnostic.path);
    unlock_queue();

    size_t written = 0;
    bool opened = false;
    {
        File file = Storage::open(path, "a");
        opened = static_cast<bool>(file);
        if (opened) {
            written = file.write(diagnostic_payload, payload_length);
            file.close();
        }
    }

    if (queue_lock && xSemaphoreTake(queue_lock, portMAX_DELAY) == pdTRUE) {
        diagnostic.bytes = written;
        if (!opened) {
            diagnostic.state = StorageDiagnosticState::Failed;
            copy_cstr(diagnostic.error, sizeof(diagnostic.error),
                      "open_failed");
        } else if (written != payload_length) {
            diagnostic.state = StorageDiagnosticState::Failed;
            copy_cstr(diagnostic.error, sizeof(diagnostic.error),
                      "short_write");
        } else {
            diagnostic.state = StorageDiagnosticState::Complete;
            diagnostic.error[0] = '\0';
        }
        unlock_queue();
    }
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
        bool did_work = false;
        const uint32_t now_ms = millis();
        if (process_mount_recovery(now_ms) ||
            process_storage_resource_recovery(now_ms)) {
            did_work = true;
        } else {
            bool have_job = false;
            size_t slot_index = SIZE_MAX;
            const bool storage_mounted = Storage::mounted();
            if (lock_queue(50)) {
                if (storage_mounted && slot_storage_available() &&
                    queued > 0 && !processing_job) {
                    slot_index = head;
                    head = (head + 1) % AC_EDF_STORAGE_QUEUE_CAPACITY;
                    queued--;
                    stats.edf_queued = queued;
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
                } else if (process_diagnostic_step()) {
                    did_work = true;
                } else if (atomic_write_service.step(
                               StorageAtomicWriteLane::Maintenance)) {
                    did_work = true;
                } else if (scan_service.step()) {
                    did_work = true;
                } else if (archive_service.step()) {
                    did_work = true;
                } else if (delete_service.step()) {
                    did_work = true;
                } else if (foreground_due && !tail_read_active &&
                           file_log_sink.step()) {
                    did_work = true;
                    file_log_burst++;
                } else if (Storage::poll(capacity_update_allowed.load())) {
                    did_work = true;
                }
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
    if (!stats.available) return false;
    if (!lock_queue()) {
        stats.queue_drops++;
        set_error("queue_lock_failed");
        log_worker_failure(LOG_WARN, "queue_lock_failed", job.path);
        return false;
    }
    if (!slot_storage_available()) {
        stats.unavailable_drops++;
        unlock_queue();
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
    if (!stats.available) return false;
    if (!lock_queue()) {
        stats.queue_drops++;
        set_error("queue_lock_failed");
        log_worker_failure(LOG_WARN, "queue_lock_failed");
        return false;
    }
    if (!slot_storage_available()) {
        stats.unavailable_drops++;
        unlock_queue();
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
    if (!queue_lock) {
        stats.available = false;
        return;
    }

    storage_resources_ready = initialize_storage_resources();
    storage_resource_retry_attempt = 0;
    storage_resource_retry_at_ms =
        storage_resources_ready
            ? 0
            : millis() + STORAGE_RESOURCE_RETRY_MIN_MS;

    const StorageStatus storage = Storage::status();
    mount_retry_attempt = 0;
    mount_retry_at_ms =
        storage.configured && storage.state == StorageState::NotPresent
            ? millis() + STORAGE_MOUNT_RETRY_MIN_MS
            : 0;

    if (!task) {
        const BaseType_t created =
            xTaskCreatePinnedToCore(task_entry, "ac_storage",
                                    AC_STORAGE_SERVICE_TASK_STACK, nullptr,
                                    AC_STORAGE_SERVICE_TASK_PRIO, &task,
                                    AC_STORAGE_SERVICE_TASK_CORE);
        if (created != pdPASS || !task) {
            stats.available = false;
            diagnostic.available = false;
            set_storage_task_available(false);
            set_error("task_create_failed");
            Log::logf(CAT_EDF, LOG_ERROR,
                      "storage worker task create failed\n");
            return;
        }
    }
    stats.initialized = true;
    stats.available = true;
    stats.read_capacity = AC_STORAGE_PREPARED_READ_CAPACITY;
    set_storage_task_available(true);

    Log::logf(CAT_STORAGE, LOG_DEBUG,
              "service ready edf_q=%u read_q=%u slot=%u psram=%s "
              "resources=%s\n",
              static_cast<unsigned>(stats.edf_capacity),
              static_cast<unsigned>(AC_STORAGE_PREPARED_READ_CAPACITY),
              static_cast<unsigned>(AC_EDF_STORAGE_SLOT_BYTES),
              stats.using_psram ? "yes" : "no",
              storage_resources_ready ? "ready" : "recovering");
}

bool request_mount_retry() {
    if (!stats.initialized || !stats.available) return false;

    bool expected = false;
    if (!mount_retry_requested.compare_exchange_strong(expected, true)) {
        return false;
    }

    wake_service_task();
    return true;
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
    if (!stats.available) return false;
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
    if (!stats.available) return false;
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
    if (!stats.available) return false;
    if (!lock_queue()) {
        stats.queue_drops++;
        set_error("queue_lock_failed");
        log_worker_failure(LOG_WARN, "queue_lock_failed");
        return false;
    }
    if (!slot_storage_available()) {
        stats.unavailable_drops++;
        unlock_queue();
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

StorageStatusPort &status_port() {
    return service_status_port;
}

StorageStreamPort &stream_port() {
    return stream_service;
}

StoragePathPort &path_port() {
    return path_service;
}

StorageAtomicWritePort &atomic_write_port() {
    return atomic_write_service;
}

StorageScanPort &scan_port() {
    return scan_service;
}

StorageBrowserPort &browser_port() {
    return browser_service;
}

StorageUploadPort &upload_port() {
    return upload_service;
}

bool take_uploaded_path(char *path, size_t path_size) {
    return upload_service.take_published_path(path, path_size);
}

StorageArchivePort &archive_port() {
    return archive_service;
}

StorageDeletePort &delete_port() {
    return delete_service;
}

bool request_diagnostic_append(const char *path,
                               const uint8_t *data,
                               size_t length) {
    if (!path || path[0] != '/' || !data || length == 0 ||
        length > AC_STORAGE_DIAGNOSTIC_PAYLOAD_CAPACITY ||
        strlen(path) >= AC_STORAGE_WRITE_PATH_MAX) {
        return false;
    }
    if (!stats.initialized) begin();
    if (!lock_queue(20)) return false;

    const bool busy = diagnostic.state == StorageDiagnosticState::Queued ||
                      diagnostic.state == StorageDiagnosticState::Writing;
    if (!stats.available || !diagnostic.available || busy) {
        unlock_queue();
        return false;
    }

    copy_cstr(diagnostic.path, sizeof(diagnostic.path), path);
    memcpy(diagnostic_payload, data, length);
    diagnostic_payload_length = length;
    diagnostic.bytes = length;
    diagnostic.error[0] = '\0';
    diagnostic.state = StorageDiagnosticState::Queued;
    unlock_queue();

    wake_service_task();
    return true;
}

StorageDiagnosticStatus diagnostic_status() {
    StorageDiagnosticStatus out;
    if (!lock_queue(20)) return out;
    out = diagnostic;
    unlock_queue();
    return out;
}

FileLogSinkPort &file_log_port() {
    (void)file_log_sink.begin(wake_service_task);
    return file_log_sink;
}

void publish_activity(const ActivitySnapshot &activity) {
    capacity_update_allowed.store(!activity.therapy_active &&
                                  !activity.realtime_stream_active);
    file_log_sink.set_rotation_allowed(!activity.therapy_active &&
                                       !activity.ota_install_active);
    const bool scan_paused =
        activity.therapy_active || activity.realtime_stream_active ||
        activity.foreground_report_demand || activity.ota_install_active;
    const bool maintenance_paused =
        scan_paused || activity.export_work_claimed;
    const bool upload_paused = activity.therapy_active ||
                               activity.ota_install_active;

    upload_service.set_paused(upload_paused);
    scan_service.set_paused(scan_paused);
    archive_service.set_paused(maintenance_paused);
    delete_service.set_paused(maintenance_paused);
    wake_service_task();
}

StorageWorkloadSnapshot workload_snapshot() {
    StorageWorkloadSnapshot out;
    const bool upload_active = upload_service.active();

    if (!lock_queue()) return out;

    refresh_read_status_locked();
    out.valid = true;
    out.available = stats.available;
    out.busy = processing_job || processing_read || upload_active ||
               diagnostic.state == StorageDiagnosticState::Queued ||
               diagnostic.state == StorageDiagnosticState::Writing;
    out.maintenance_active =
        maintenance_owner.load() != MaintenanceOwner::None;
    out.edf_queued = queued;
    out.open_file_count = stats.open_file_count;
    unlock_queue();
    return out;
}

StorageEdfStatusSnapshot edf_status_snapshot() {
    StorageEdfStatusSnapshot out;
    const bool upload_active = upload_service.active();

    if (!lock_queue()) return out;

    refresh_read_status_locked();
    out.busy = processing_job || processing_read || upload_active ||
               diagnostic.state == StorageDiagnosticState::Queued ||
               diagnostic.state == StorageDiagnosticState::Writing;
    out.capacity = stats.edf_capacity;
    out.queued = queued;
    out.open_file_count = stats.open_file_count;
    out.records_written = stats.records_written;
    out.identification_jobs = stats.identification_jobs;
    out.queue_drops = stats.queue_drops;
    out.patch_errors = stats.patch_errors;
#if AC_STACK_PROFILE_ENABLED
    if (task) out.stack_high_water_words = uxTaskGetStackHighWaterMark(task);
#endif
    copy_cstr(out.last_error, sizeof(out.last_error), stats.last_error);
    unlock_queue();
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

const char *storage_diagnostic_state_name(StorageDiagnosticState state) {
    switch (state) {
        case StorageDiagnosticState::Queued: return "queued";
        case StorageDiagnosticState::Writing: return "writing";
        case StorageDiagnosticState::Complete: return "complete";
        case StorageDiagnosticState::Failed: return "failed";
        case StorageDiagnosticState::Idle:
        default: return "idle";
    }
}

}  // namespace aircannect
