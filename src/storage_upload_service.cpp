#include "storage_upload_service.h"

#include <algorithm>
#include <Arduino.h>
#include <ctype.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include <FS.h>
#include <mbedtls/sha256.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_internal.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr const char *UPLOAD_DIR = "/aircannect/.uploads";
static constexpr size_t WRITE_STEP_BYTES = 4096;
static constexpr uint32_t CLIENT_IDLE_TIMEOUT_MS = 2 * 60 * 1000;

bool normalize_sha256(const std::string &value, char out[65]) {
    out[0] = '\0';
    if (value.empty()) return true;
    if (value.size() != 64) return false;

    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!isxdigit(c)) return false;
        out[i] = static_cast<char>(toupper(c));
    }
    out[64] = '\0';
    return true;
}

void hash_to_hex(const uint8_t hash[32], char out[65]) {
    static constexpr char DIGITS[] = "0123456789ABCDEF";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = DIGITS[(hash[i] >> 4) & 0x0F];
        out[i * 2 + 1] = DIGITS[hash[i] & 0x0F];
    }
    out[64] = '\0';
}

bool parent_directory(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    copy_cstr(out, out_size, path);

    char *slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) {
        out[1] = '\0';
        return true;
    }
    *slash = '\0';
    return true;
}

bool upload_artifact_name(const char *name) {
    return name && strncmp(name, "receive-", 8) == 0 &&
           strstr(name, ".part") != nullptr;
}

}  // namespace

struct StorageUploadService::Job {
    StorageUploadStatus status;
    StorageUploadState resume_state = StorageUploadState::Idle;
    StorageUploadConflict conflict = StorageUploadConflict::Fail;
    uint64_t free_reserve_bytes = 0;
    uint32_t generation = 0;
    uint32_t last_activity_ms = 0;
    bool cancel_requested = false;

    File output;
    char temporary_path[AC_STORAGE_PATH_MAX] = {};
    char expected_sha256[65] = {};
    mbedtls_sha256_context sha;
    bool sha_started = false;

    std::shared_ptr<const LargeByteBuffer> pending;
    size_t pending_offset = 0;
    OperationTicket publication_ticket;

    Job() { mbedtls_sha256_init(&sha); }
    ~Job() {
        if (output) output.close();
        mbedtls_sha256_free(&sha);
    }
};

const char *storage_upload_state_name(StorageUploadState state) {
    switch (state) {
        case StorageUploadState::Idle: return "idle";
        case StorageUploadState::Preparing: return "preparing";
        case StorageUploadState::Ready: return "ready";
        case StorageUploadState::Writing: return "writing";
        case StorageUploadState::Paused: return "paused";
        case StorageUploadState::Publishing: return "publishing";
        case StorageUploadState::Done: return "done";
        case StorageUploadState::Cancelled: return "cancelled";
        case StorageUploadState::Error: return "error";
    }
    return "unknown";
}

StorageUploadService::~StorageUploadService() {
    if (job_) {
        job_->~Job();
        Memory::free(job_);
    }
    release_maintenance_locked();
    if (lock_) vSemaphoreDelete(lock_);
    if (status_mutex_) vSemaphoreDelete(status_mutex_);
}

bool StorageUploadService::begin(
    WakeCallback wake,
    StorageAtomicWritePort &atomic_write_port,
    ClaimMaintenanceCallback claim_maintenance,
    ReleaseMaintenanceCallback release_maintenance) {
    wake_ = wake;
    atomic_write_port_ = &atomic_write_port;
    claim_maintenance_ = claim_maintenance;
    release_maintenance_ = release_maintenance;
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    if (!status_mutex_) {
        status_mutex_ = xSemaphoreCreateMutexStatic(&status_mutex_storage_);
    }
    if (!lock_ || !status_mutex_) return false;

    if (!job_) {
        void *memory = Memory::alloc_large(sizeof(Job), false);
        if (memory) job_ = new (memory) Job();
    }
    if (job_) return true;

    Log::logf(CAT_STORAGE, LOG_ERROR, "upload service unavailable\n");
    return false;
}

void StorageUploadService::set_task_available(bool available) {
    task_available_.store(available, std::memory_order_release);
    if (available) wake();
}

void StorageUploadService::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_release);
    if (!paused) wake();
}

bool StorageUploadService::ready() const {
    return lock_ && status_mutex_ && job_ && atomic_write_port_ &&
           task_available_.load(std::memory_order_acquire);
}

bool StorageUploadService::lock(uint32_t timeout_ms) const {
    return lock_ &&
           xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageUploadService::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

bool StorageUploadService::status_lock(uint32_t timeout_ms) const {
    return status_mutex_ &&
           xSemaphoreTake(status_mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageUploadService::status_unlock() const {
    if (status_mutex_) xSemaphoreGive(status_mutex_);
}

void StorageUploadService::wake() const {
    if (wake_) wake_();
}

uint32_t StorageUploadService::next_id_locked() {
    next_id_++;
    if (next_id_ == 0) next_id_++;
    return next_id_;
}

bool StorageUploadService::claim_maintenance_locked() {
    if (maintenance_claimed_) return true;
    if (!claim_maintenance_ || !claim_maintenance_()) return false;

    maintenance_claimed_ = true;
    return true;
}

void StorageUploadService::release_maintenance_locked() {
    if (!maintenance_claimed_) return;
    if (release_maintenance_) release_maintenance_();
    maintenance_claimed_ = false;
}

void StorageUploadService::publish_status_locked() {
    if (!job_ || !status_mutex_ ||
        xSemaphoreTake(status_mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    status_snapshot_ = job_->status;
    status_snapshot_valid_ = status_snapshot_.id != 0;
    status_unlock();
}

void StorageUploadService::queue_publication_notice_locked() {
    if (!status_mutex_ ||
        xSemaphoreTake(status_mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    copy_cstr(publication_notice_path_, sizeof(publication_notice_path_),
              job_->status.path);
    publication_notice_pending_ = publication_notice_path_[0] != '\0';
    status_unlock();
}

bool StorageUploadService::take_published_path(char *path,
                                               size_t path_size) {
    if (!path || path_size == 0) return false;
    path[0] = '\0';
    if (!status_lock()) return false;

    const bool available = publication_notice_pending_;
    if (available) {
        copy_cstr(path, path_size, publication_notice_path_);
        publication_notice_path_[0] = '\0';
        publication_notice_pending_ = false;
    }
    status_unlock();
    return available;
}

StorageUploadStartResult StorageUploadService::start(
    const StorageUploadStartCommand &command) {
    StorageUploadStartResult result;
    char expected_sha256[65] = {};
    if (!command.valid() || command.path.size() >= AC_STORAGE_PATH_MAX ||
        command.expected_sha256.size() > 64 ||
        !normalize_sha256(command.expected_sha256, expected_sha256)) {
        copy_cstr(result.error, sizeof(result.error), "invalid_request");
        return result;
    }
    if (!ready() || !lock()) {
        result.admission = OperationAdmission::Busy;
        copy_cstr(result.error, sizeof(result.error), "service_busy");
        return result;
    }
    if (job_->status.active()) {
        result.admission = OperationAdmission::Busy;
        copy_cstr(result.error, sizeof(result.error), "upload_active");
        unlock();
        return result;
    }

    clear_job_locked();
    const uint32_t id = next_id_locked();
    job_->status.id = id;
    job_->status.state = StorageUploadState::Preparing;
    job_->status.total_bytes = command.total_size;
    copy_cstr(job_->status.path, sizeof(job_->status.path),
              command.path.c_str());
    job_->conflict = command.conflict;
    job_->free_reserve_bytes = command.free_reserve_bytes;
    job_->generation = command.generation;
    job_->last_activity_ms = millis();
    copy_cstr(job_->expected_sha256, sizeof(job_->expected_sha256),
              expected_sha256);
    publish_status_locked();

    result.admission = OperationAdmission::Accepted;
    result.id = id;
    result.chunk_size = AC_STORAGE_UPLOAD_CHUNK_BYTES;
    unlock();

    wake();
    return result;
}

StorageUploadChunkResult StorageUploadService::submit(
    const StorageUploadChunkCommand &command) {
    StorageUploadChunkResult result;
    if (!command.valid()) {
        copy_cstr(result.error, sizeof(result.error), "invalid_chunk");
        return result;
    }
    if (!ready() || !lock()) {
        result.admission = OperationAdmission::Busy;
        copy_cstr(result.error, sizeof(result.error), "service_busy");
        return result;
    }

    result.committed_bytes = job_->status.committed_bytes;
    if (job_->status.id != command.id) {
        copy_cstr(result.error, sizeof(result.error), "unknown_upload");
        unlock();
        return result;
    }
    if (paused_.load(std::memory_order_acquire) ||
        job_->status.state == StorageUploadState::Paused) {
        result.admission = OperationAdmission::Busy;
        copy_cstr(result.error, sizeof(result.error), "paused");
        unlock();
        return result;
    }
    if (job_->status.state != StorageUploadState::Ready || job_->pending) {
        result.admission = OperationAdmission::Busy;
        copy_cstr(result.error, sizeof(result.error), "chunk_pending");
        unlock();
        return result;
    }
    if (command.offset != job_->status.committed_bytes) {
        copy_cstr(result.error, sizeof(result.error), "offset_mismatch");
        unlock();
        return result;
    }
    if (command.offset > job_->status.total_bytes ||
        command.bytes->size() >
            job_->status.total_bytes - command.offset) {
        copy_cstr(result.error, sizeof(result.error), "chunk_too_large");
        unlock();
        return result;
    }

    job_->pending = command.bytes;
    job_->pending_offset = 0;
    job_->last_activity_ms = millis();
    set_state_locked(StorageUploadState::Writing);
    result.admission = OperationAdmission::Accepted;
    unlock();

    wake();
    return result;
}

StorageUploadStatusRead StorageUploadService::status(
    uint32_t id, StorageUploadStatus &status_out) const {
    status_out = {};
    if (!status_lock()) return StorageUploadStatusRead::Busy;
    if (!status_snapshot_valid_ ||
        (id != 0 && status_snapshot_.id != id)) {
        status_unlock();
        return StorageUploadStatusRead::NotFound;
    }

    status_out = status_snapshot_;
    status_unlock();
    return StorageUploadStatusRead::Found;
}

bool StorageUploadService::active() const {
    if (!status_lock()) return true;

    const bool out = status_snapshot_valid_ && status_snapshot_.active();
    status_unlock();
    return out;
}

bool StorageUploadService::cancel(uint32_t id) {
    if (id == 0 || !job_ || !lock(50)) return false;
    if (job_->status.id != id || !job_->status.active()) {
        unlock();
        return false;
    }

    job_->cancel_requested = true;
    job_->last_activity_ms = millis();
    unlock();

    wake();
    return true;
}

bool StorageUploadService::prepare_locked(const char *&error) {
    const StorageStatus storage = Storage::status();
    if (!storage.mounted) {
        error = "storage_not_mounted";
        return false;
    }
    if (job_->status.total_bytes > UINT64_MAX - job_->free_reserve_bytes ||
        storage.free_bytes <
            job_->status.total_bytes + job_->free_reserve_bytes) {
        error = "not_enough_storage";
        return false;
    }

    if (!validate_target_locked(error)) return false;
    if (!Storage::ensure_dir("/aircannect") ||
        !Storage::ensure_dir(UPLOAD_DIR)) {
        error = "upload_dir_failed";
        return false;
    }

    const int written = snprintf(job_->temporary_path,
                                 sizeof(job_->temporary_path),
                                 "%s/receive-%08lx.part", UPLOAD_DIR,
                                 static_cast<unsigned long>(job_->status.id));
    if (written <= 0 ||
        static_cast<size_t>(written) >= sizeof(job_->temporary_path)) {
        error = "temporary_path_failed";
        return false;
    }
    if (!Storage::remove(job_->temporary_path)) {
        error = "temporary_cleanup_failed";
        return false;
    }

    job_->output = Storage::open(job_->temporary_path, "w");
    if (!job_->output) {
        error = "temporary_open_failed";
        return false;
    }
    (void)job_->output.setBufferSize(4096);
    mbedtls_sha256_starts(&job_->sha, 0);
    job_->sha_started = true;
    if (job_->status.total_bytes == 0) return finalize_locked(error);

    set_state_locked(StorageUploadState::Ready);
    return true;
}

bool StorageUploadService::pause_locked(const char *&error) {
    if (job_->output) {
        job_->output.flush();
        job_->output.close();
    }

    const uint64_t expected_size =
        job_->status.committed_bytes + job_->pending_offset;
    if (job_->temporary_path[0] &&
        !temporary_size_matches_locked(expected_size)) {
        error = "temporary_file_changed";
        return false;
    }

    job_->resume_state = job_->status.state;
    set_state_locked(StorageUploadState::Paused);
    return true;
}

bool StorageUploadService::resume_locked(const char *&error) {
    if (!validate_target_locked(error)) return false;

    if (job_->temporary_path[0]) {
        const uint64_t expected_size =
            job_->status.committed_bytes + job_->pending_offset;
        if (!temporary_size_matches_locked(expected_size)) {
            error = "temporary_file_changed";
            return false;
        }

        job_->output = Storage::open(job_->temporary_path, "a");
        if (!job_->output) {
            error = "temporary_reopen_failed";
            return false;
        }
        (void)job_->output.setBufferSize(4096);
    }

    set_state_locked(job_->resume_state);
    job_->resume_state = StorageUploadState::Idle;
    return true;
}

bool StorageUploadService::validate_target_locked(const char *&error) const {
    char parent[AC_STORAGE_PATH_MAX] = {};
    if (!parent_directory(job_->status.path, parent, sizeof(parent))) {
        error = "bad_parent";
        return false;
    }

    File parent_dir = Storage::open(parent, "r");
    const bool parent_ok = parent_dir && parent_dir.isDirectory();
    if (parent_dir) parent_dir.close();
    if (!parent_ok) {
        error = "parent_not_found";
        return false;
    }
    if (job_->conflict == StorageUploadConflict::Fail &&
        Storage::exists(job_->status.path)) {
        error = "destination_exists";
        return false;
    }
    return true;
}

bool StorageUploadService::temporary_size_matches_locked(
    uint64_t expected_size) const {
    File temporary = Storage::open(job_->temporary_path, "r");
    const bool matches = temporary && !temporary.isDirectory() &&
                         temporary.size() == expected_size;
    if (temporary) temporary.close();
    return matches;
}

bool StorageUploadService::write_locked(const char *&error) {
    if (!job_->output || !job_->pending || !job_->sha_started ||
        job_->pending_offset >= job_->pending->size()) {
        error = "write_state_invalid";
        return false;
    }

    const size_t remaining = job_->pending->size() - job_->pending_offset;
    const size_t wanted = std::min(remaining, WRITE_STEP_BYTES);
    const uint8_t *data = job_->pending->data() + job_->pending_offset;
    const size_t written = job_->output.write(data, wanted);
    if (written != wanted) {
        error = "write_failed";
        return false;
    }

    mbedtls_sha256_update(&job_->sha, data, written);
    job_->pending_offset += written;
    job_->last_activity_ms = millis();
    if (job_->pending_offset != job_->pending->size()) return true;

    job_->status.committed_bytes += job_->pending->size();
    job_->pending.reset();
    job_->pending_offset = 0;
    if (job_->status.committed_bytes == job_->status.total_bytes) {
        return finalize_locked(error);
    }

    set_state_locked(StorageUploadState::Ready);
    return true;
}

bool StorageUploadService::finalize_locked(const char *&error) {
    if (!job_->output || !job_->sha_started) {
        error = "finalize_state_invalid";
        return false;
    }

    job_->output.flush();
    job_->output.close();

    uint8_t hash[32] = {};
    char computed[65] = {};
    mbedtls_sha256_finish(&job_->sha, hash);
    job_->sha_started = false;
    hash_to_hex(hash, computed);
    if (job_->expected_sha256[0] &&
        strcmp(job_->expected_sha256, computed) != 0) {
        error = "sha256_mismatch";
        return false;
    }
    if (!validate_target_locked(error)) return false;

    return submit_publication_locked(error);
}

bool StorageUploadService::submit_publication_locked(const char *&error) {
    if (job_->publication_ticket.valid()) return true;

    next_publish_generation_++;
    if (next_publish_generation_ == 0) next_publish_generation_++;

    StorageAtomicWriteCommand command;
    command.path = job_->status.path;
    command.staged_path = job_->temporary_path;
    command.staged_size = job_->status.total_bytes;
    command.free_reserve_bytes = job_->free_reserve_bytes;
    command.lane = StorageAtomicWriteLane::Foreground;
    command.generation = next_publish_generation_;
    command.replace_existing =
        job_->conflict == StorageUploadConflict::Replace;

    const OperationSubmission submission =
        atomic_write_port_->request_write(command);
    if (submission.accepted()) {
        job_->publication_ticket = submission.ticket;
        set_state_locked(StorageUploadState::Publishing);
        return true;
    }
    if (submission.admission == OperationAdmission::Busy) {
        set_state_locked(StorageUploadState::Publishing);
        return true;
    }

    error = "publish_rejected";
    return false;
}

bool StorageUploadService::poll_publication_locked(const char *&error) {
    if (!job_->publication_ticket.valid()) {
        return submit_publication_locked(error);
    }

    StorageAtomicWriteCompletion completion;
    if (!atomic_write_port_->take_completion(job_->publication_ticket,
                                             completion)) {
        return true;
    }
    job_->publication_ticket = {};
    if (completion.outcome.disposition !=
        OperationDisposition::Succeeded) {
        error = completion.error[0] ? completion.error : "publish_failed";
        return false;
    }

    queue_publication_notice_locked();
    finish_locked(StorageUploadState::Done);
    return true;
}

bool StorageUploadService::cleanup_abandoned_step_locked() {
    if (!cleanup_pending_ || !Storage::mounted()) return false;
    if (!Storage::exists(UPLOAD_DIR)) {
        cleanup_pending_ = false;
        return false;
    }

    File directory = Storage::open(UPLOAD_DIR, "r");
    if (!directory || !directory.isDirectory()) {
        if (directory) directory.close();
        cleanup_pending_ = false;
        return false;
    }

    StorageDirChild child;
    while (storage_read_next_dir_child(directory, child)) {
        if (child.is_dir || !upload_artifact_name(child.name)) continue;

        char path[AC_STORAGE_PATH_MAX] = {};
        const bool valid = storage_append_child_path(
            UPLOAD_DIR, child.name, path, sizeof(path));
        directory.close();
        if (valid) (void)Storage::remove(path);
        return true;
    }

    directory.close();
    cleanup_pending_ = false;
    return false;
}

void StorageUploadService::set_state_locked(StorageUploadState state) {
    job_->status.state = state;
    job_->status.error[0] = '\0';
    publish_status_locked();
}

void StorageUploadService::finish_locked(StorageUploadState state,
                                         const char *error) {
    close_input_locked();
    job_->pending.reset();
    job_->pending_offset = 0;
    job_->status.state = state;
    copy_cstr(job_->status.error, sizeof(job_->status.error), error);
    job_->last_activity_ms = millis();
    release_maintenance_locked();
    publish_status_locked();

    if (state == StorageUploadState::Error && error) {
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "upload failed path=%s error=%s\n",
                  job_->status.path, error);
    }
}

void StorageUploadService::close_input_locked() {
    if (job_->output) job_->output.close();
    job_->sha_started = false;
}

void StorageUploadService::clear_job_locked() {
    if (!job_) return;
    job_->~Job();
    new (job_) Job();
}

void StorageUploadService::cancel_locked() {
    if (job_->publication_ticket.valid()) {
        StorageAtomicWriteCompletion completion;
        if (atomic_write_port_->take_completion(job_->publication_ticket,
                                                completion) &&
            completion.outcome.disposition ==
                OperationDisposition::Succeeded) {
            job_->publication_ticket = {};
            finish_locked(StorageUploadState::Done);
            return;
        }

        (void)atomic_write_port_->abandon(job_->publication_ticket);
        job_->publication_ticket = {};
    }

    close_input_locked();
    if (job_->temporary_path[0]) {
        (void)Storage::remove(job_->temporary_path);
    }
    finish_locked(StorageUploadState::Cancelled);
}

bool StorageUploadService::step() {
    if (!ready() || !lock(50)) return false;

    if (!job_->status.active()) {
        if (!cleanup_pending_ || !claim_maintenance_locked()) {
            unlock();
            return false;
        }
        const bool cleaned = cleanup_abandoned_step_locked();
        release_maintenance_locked();
        unlock();
        return cleaned;
    }
    if (job_->cancel_requested) {
        if (!claim_maintenance_locked()) {
            unlock();
            return false;
        }
        cancel_locked();
        unlock();
        return true;
    }

    const bool paused = paused_.load(std::memory_order_acquire);
    const char *pause_error = nullptr;
    if (paused && job_->status.state != StorageUploadState::Publishing) {
        if (job_->status.state == StorageUploadState::Paused) {
            release_maintenance_locked();
            unlock();
            return false;
        }
        if (!claim_maintenance_locked()) {
            unlock();
            return false;
        }
        if (job_->status.state != StorageUploadState::Paused) {
            if (!pause_locked(pause_error)) {
                finish_locked(StorageUploadState::Error,
                              pause_error ? pause_error : "pause_failed");
                unlock();
                return true;
            }
        }
        release_maintenance_locked();
        unlock();
        return false;
    }
    if (!paused && job_->status.state == StorageUploadState::Paused) {
        if (!claim_maintenance_locked()) {
            unlock();
            return false;
        }
        if (!resume_locked(pause_error)) {
            finish_locked(StorageUploadState::Error,
                          pause_error ? pause_error : "resume_failed");
            unlock();
            return true;
        }
    }

    const uint32_t now_ms = millis();
    if (job_->status.state == StorageUploadState::Ready) {
        release_maintenance_locked();
        if (static_cast<int32_t>(now_ms - job_->last_activity_ms) <
            static_cast<int32_t>(CLIENT_IDLE_TIMEOUT_MS)) {
            unlock();
            return false;
        }
        if (!claim_maintenance_locked()) {
            unlock();
            return false;
        }
        cancel_locked();
        unlock();
        return true;
    }

    if (!claim_maintenance_locked()) {
        unlock();
        return false;
    }

    const char *error = nullptr;
    bool ok = true;
    bool worked = false;
    switch (job_->status.state) {
        case StorageUploadState::Preparing:
            ok = prepare_locked(error);
            worked = true;
            break;
        case StorageUploadState::Writing:
            ok = write_locked(error);
            worked = true;
            break;
        case StorageUploadState::Publishing:
            ok = poll_publication_locked(error);
            worked = job_->publication_ticket.valid() ||
                     job_->status.state != StorageUploadState::Publishing;
            break;
        default:
            break;
    }

    if (!ok) {
        if (job_->temporary_path[0]) {
            (void)Storage::remove(job_->temporary_path);
        }
        finish_locked(StorageUploadState::Error,
                      error ? error : "upload_failed");
    }

    const bool retain_maintenance =
        job_->status.state == StorageUploadState::Writing ||
        (job_->status.state == StorageUploadState::Publishing &&
         job_->publication_ticket.valid());
    if (!retain_maintenance) release_maintenance_locked();

    unlock();
    return worked;
}

}  // namespace aircannect
