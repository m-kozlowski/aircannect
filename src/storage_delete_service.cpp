#include "storage_delete_service.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_directory.h"
#include "storage_internal.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t DELETE_MAX_DEPTH = 16;
static constexpr uint32_t DELETE_STEP_SLICE_US = 10 * 1000;
static constexpr size_t DELETE_PATH_BYTES_CAPACITY =
    AC_STORAGE_MAX_SELECTIONS * AC_STORAGE_PATH_MAX;

void log_delete_alloc_failed(const char *context, size_t bytes) {
    Log::logf(CAT_STORAGE,
              LOG_ERROR,
              "[DELETE] allocation failed context=%s bytes=%u\n",
              context ? context : "--",
              static_cast<unsigned>(bytes));
}

}  // namespace

struct StorageDeleteService::WalkFrame {
    char path[AC_STORAGE_PATH_MAX] = {};
    bool opened = false;
    File dir;
};

const char *storage_delete_state_name(StorageDeleteState state) {
    switch (state) {
        case StorageDeleteState::Idle: return "idle";
        case StorageDeleteState::Deleting: return "deleting";
        case StorageDeleteState::Done: return "done";
        case StorageDeleteState::Error: return "error";
    }
    return "unknown";
}

StorageDeleteService::~StorageDeleteService() {
    if (walk_stack_) {
        for (size_t i = 0; i < walk_capacity_; ++i) {
            walk_stack_[i].~WalkFrame();
        }
        Memory::free(walk_stack_);
    }
    if (path_bytes_) Memory::free(path_bytes_);
    release_maintenance_locked();
    if (lock_) vSemaphoreDelete(lock_);
}

bool StorageDeleteService::begin(
    WakeCallback wake,
    ClaimMaintenanceCallback claim_maintenance,
    ReleaseMaintenanceCallback release_maintenance) {
    if (lock_) return ready();

    wake_ = wake;
    claim_maintenance_ = claim_maintenance;
    release_maintenance_ = release_maintenance;
    lock_ = xSemaphoreCreateMutex();
    if (!lock_ || !published_status_.begin(status_) || !allocate_owners()) {
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[DELETE] service unavailable\n");
        return false;
    }
    return true;
}

void StorageDeleteService::set_task_available(bool available) {
    task_available_.store(available);
    if (available) wake();
}

void StorageDeleteService::set_paused(bool paused) {
    const bool changed = paused_.exchange(paused) != paused;
    if (!changed) return;

    pause_transition_pending_.store(paused);
    wake();
}

bool StorageDeleteService::ready() const {
    return lock_ && path_bytes_ && walk_stack_ && claim_maintenance_ &&
           release_maintenance_ && task_available_.load();
}

bool StorageDeleteService::allocate_owners() {
    path_bytes_capacity_ = DELETE_PATH_BYTES_CAPACITY;
    path_bytes_ = static_cast<char *>(
        Memory::alloc_large(path_bytes_capacity_, true));
    if (!path_bytes_) {
        log_delete_alloc_failed("paths", path_bytes_capacity_);
        path_bytes_capacity_ = 0;
        return false;
    }

    walk_capacity_ = DELETE_MAX_DEPTH;
    walk_stack_ = static_cast<WalkFrame *>(
        Memory::alloc_large(sizeof(WalkFrame) * walk_capacity_, true));
    if (!walk_stack_) {
        log_delete_alloc_failed("walk_stack",
                                sizeof(WalkFrame) * walk_capacity_);
        walk_capacity_ = 0;
        return false;
    }
    for (size_t i = 0; i < walk_capacity_; ++i) {
        new (&walk_stack_[i]) WalkFrame();
    }
    return true;
}

void StorageDeleteService::wake() const {
    if (wake_) wake_();
}

bool StorageDeleteService::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageDeleteService::unlock() const {
    if (status_dirty_ && published_status_.publish(status_)) {
        status_dirty_ = false;
    }
    if (lock_) xSemaphoreGive(lock_);
}

bool StorageDeleteService::claim_maintenance_locked() {
    if (maintenance_claimed_) return true;
    if (!claim_maintenance_ || !claim_maintenance_()) return false;

    maintenance_claimed_ = true;
    return true;
}

void StorageDeleteService::release_maintenance_locked() {
    if (!maintenance_claimed_) return;

    maintenance_claimed_ = false;
    if (release_maintenance_) release_maintenance_();
}

void StorageDeleteService::touch_status_locked() {
    status_.updated_ms = nonzero_millis(millis());
    status_dirty_ = true;
}

void StorageDeleteService::set_error_locked(const char *error) {
    close_walk_locked();
    active_.store(false);
    release_maintenance_locked();
    status_.state = StorageDeleteState::Error;
    copy_cstr(status_.error, sizeof(status_.error), error);
    touch_status_locked();
    Log::logf(CAT_STORAGE, LOG_WARN, "[DELETE] error=%s\n",
              status_.error[0] ? status_.error : "error");
}

void StorageDeleteService::close_walk_locked() {
    if (!walk_stack_) return;
    for (size_t i = 0; i < walk_depth_; ++i) {
        if (walk_stack_[i].opened) {
            walk_stack_[i].dir.close();
            walk_stack_[i].opened = false;
        }
    }
    walk_depth_ = 0;
}

bool StorageDeleteService::append_root_locked(const char *path) {
    if (!path || status_.roots >= AC_STORAGE_MAX_SELECTIONS) {
        set_error_locked("too_many_items");
        return false;
    }
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_PATH_MAX ||
        path_bytes_len_ + len + 1 > path_bytes_capacity_) {
        set_error_locked("metadata_alloc");
        return false;
    }
    root_offsets_[status_.roots++] = static_cast<uint32_t>(path_bytes_len_);
    memcpy(path_bytes_ + path_bytes_len_, path, len + 1);
    path_bytes_len_ += len + 1;
    return true;
}

bool StorageDeleteService::push_dir_locked(const char *path) {
    if (!walk_stack_ || walk_depth_ >= walk_capacity_) {
        set_error_locked("max_depth");
        return false;
    }
    WalkFrame &frame = walk_stack_[walk_depth_++];
    frame = WalkFrame();
    copy_cstr(frame.path, sizeof(frame.path), path);
    return true;
}

bool StorageDeleteService::ensure_dir_open_locked(WalkFrame &frame) {
    if (frame.opened) return true;
    frame.dir = Storage::open(frame.path, "r");
    if (!frame.dir) {
        set_error_locked("not_found");
        return false;
    }
    if (!frame.dir.isDirectory()) {
        frame.dir.close();
        set_error_locked("not_directory");
        return false;
    }
    frame.opened = true;
    return true;
}

bool StorageDeleteService::start_selected(const char *base_path,
                                          const char *const *names,
                                          size_t count,
                                          uint32_t *id_out,
                                          char *error_out,
                                          size_t error_out_size) {
    if (id_out) *id_out = 0;
    copy_cstr(error_out, error_out_size, "");
    if (!ready()) {
        copy_cstr(error_out, error_out_size, "delete_unavailable");
        return false;
    }
    if (!storage_user_path_valid(base_path)) {
        copy_cstr(error_out, error_out_size, "bad_path");
        return false;
    }
    if (!names || count == 0 || count > AC_STORAGE_MAX_SELECTIONS) {
        copy_cstr(error_out, error_out_size, "bad_selection");
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!storage_valid_child_name(names[i])) {
            copy_cstr(error_out, error_out_size, "bad_name");
            return false;
        }
    }

    char normalized_base[AC_STORAGE_PATH_MAX] = {};
    copy_cstr(normalized_base, sizeof(normalized_base), base_path);
    storage_normalize_path(normalized_base);
    for (size_t i = 0; i < count; ++i) {
        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(normalized_base,
                                       names[i],
                                       child_path,
                                       sizeof(child_path))) {
            copy_cstr(error_out, error_out_size, "bad_child_path");
            return false;
        }
        if (!storage_user_path_valid(child_path) ||
            strcmp(child_path, "/") == 0) {
            copy_cstr(error_out, error_out_size, "bad_child_path");
            return false;
        }
    }

    if (!lock(50)) {
        copy_cstr(error_out, error_out_size, "busy");
        return false;
    }
    if (paused_.load()) {
        copy_cstr(error_out, error_out_size, "storage_busy");
        unlock();
        return false;
    }
    if (status_.state == StorageDeleteState::Deleting) {
        copy_cstr(error_out, error_out_size, "delete_busy");
        unlock();
        return false;
    }
    if (!claim_maintenance_locked()) {
        copy_cstr(error_out, error_out_size, "storage_busy");
        unlock();
        return false;
    }

    close_walk_locked();
    active_.store(false);
    base_checked_ = false;
    path_bytes_len_ = 0;
    current_root_ = 0;
    memset(root_offsets_, 0, sizeof(root_offsets_));
    status_ = StorageDeleteStatus();
    status_.state = StorageDeleteState::Idle;
    touch_status_locked();

    status_.state = StorageDeleteState::Deleting;
    status_.id = next_id_++;
    if (status_.id == 0) status_.id = next_id_++;
    copy_cstr(status_.base_path, sizeof(status_.base_path), normalized_base);
    status_.started_ms = nonzero_millis(millis());
    status_.updated_ms = status_.started_ms;
    status_dirty_ = true;

    for (size_t i = 0; i < count; ++i) {
        char child_path[AC_STORAGE_PATH_MAX] = {};
        storage_append_child_path(status_.base_path,
                                  names[i],
                                  child_path,
                                  sizeof(child_path));
        if (!append_root_locked(child_path)) {
            copy_cstr(error_out, error_out_size, status_.error);
            unlock();
            return false;
        }
    }

    active_.store(true);
    if (id_out) *id_out = status_.id;
    unlock();
    wake();
    return true;
}

bool StorageDeleteService::status(StorageDeleteStatus &out,
                                  uint32_t timeout_ms) const {
    return published_status_.read(out, timeout_ms);
}

bool StorageDeleteService::validate_base_locked() {
    File base_dir = Storage::open(status_.base_path, "r");
    const bool valid = base_dir && base_dir.isDirectory();
    if (base_dir) base_dir.close();

    if (!valid) {
        set_error_locked("not_directory");
        return false;
    }
    base_checked_ = true;
    return true;
}

bool StorageDeleteService::step() {
    if (!active_.load()) return false;
    if (paused_.load()) {
        if (!pause_transition_pending_.exchange(false)) return false;
        if (!lock(50)) return false;

        close_walk_locked();
        touch_status_locked();
        unlock();
        return true;
    }
    if (!lock(50)) return false;
    if (status_.state != StorageDeleteState::Deleting) {
        unlock();
        return false;
    }
    if (!Storage::mounted()) {
        set_error_locked("storage_unavailable");
        unlock();
        return true;
    }
    if (!base_checked_ && !validate_base_locked()) {
        unlock();
        return true;
    }

    const uint32_t slice_started_us = micros();

    while (status_.state == StorageDeleteState::Deleting) {
        if (!delete_next_locked()) {
            unlock();
            return true;
        }
        if (static_cast<uint32_t>(micros() - slice_started_us) >=
            DELETE_STEP_SLICE_US) {
            break;
        }
    }
    unlock();
    return true;
}

bool StorageDeleteService::delete_next_locked() {
    if (walk_depth_ > 0) return delete_dir_step_locked();
    if (current_root_ >= status_.roots) return finish_done_locked();

    const char *path = path_bytes_ + root_offsets_[current_root_];
    bool exists = false;
    bool is_dir = false;
    {
        File node = Storage::open(path, "r");
        if (node) {
            exists = true;
            is_dir = node.isDirectory();
            node.close();
        }
    }

    if (!exists) {
        status_.roots_done++;
        current_root_++;
        touch_status_locked();
        return true;
    }
    if (is_dir) {
        if (!push_dir_locked(path)) return false;
        return true;
    }
    if (!Storage::remove(path)) {
        set_error_locked("remove_failed");
        return false;
    }
    status_.files_deleted++;
    status_.roots_done++;
    current_root_++;
    touch_status_locked();
    return true;
}

bool StorageDeleteService::delete_dir_step_locked() {
    WalkFrame &frame = walk_stack_[walk_depth_ - 1];
    if (!ensure_dir_open_locked(frame)) return false;

    StorageDirChild child;
    if (!storage_read_next_dir_child(frame.dir, child)) {
        {
            frame.dir.close();
            frame.opened = false;
        }
        char dir_path[AC_STORAGE_PATH_MAX] = {};
        copy_cstr(dir_path, sizeof(dir_path), frame.path);
        walk_depth_--;
        if (!Storage::rmdir(dir_path)) {
            set_error_locked("rmdir_failed");
            return false;
        }
        status_.dirs_deleted++;
        if (walk_depth_ == 0) {
            status_.roots_done++;
            current_root_++;
        }
        touch_status_locked();
        return true;
    }

    char child_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(frame.path,
                                   child.name,
                                   child_path,
                                   sizeof(child_path)) ||
        !storage_user_path_valid(child_path)) {
        set_error_locked("bad_child_path");
        return false;
    }
    if (child.is_dir) {
        if (!push_dir_locked(child_path)) return false;
        return true;
    }
    if (!Storage::remove(child_path)) {
        set_error_locked("remove_failed");
        return false;
    }
    status_.files_deleted++;
    touch_status_locked();
    return true;
}

bool StorageDeleteService::finish_done_locked() {
    close_walk_locked();
    active_.store(false);
    release_maintenance_locked();
    status_.state = StorageDeleteState::Done;
    touch_status_locked();
    Log::logf(CAT_STORAGE, LOG_INFO,
              "[DELETE] done roots=%u files=%u dirs=%u\n",
              static_cast<unsigned>(status_.roots),
              static_cast<unsigned>(status_.files_deleted),
              static_cast<unsigned>(status_.dirs_deleted));
    return true;
}

}  // namespace aircannect
