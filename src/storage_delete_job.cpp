#include "storage_delete_job.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_directory.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t DELETE_MAX_DEPTH = 16;
static constexpr uint32_t DELETE_STEP_SLICE_US = 10 * 1000;
static constexpr size_t DELETE_INITIAL_PATH_BYTES = 1024;

void log_delete_alloc_failed(const char *context, size_t bytes) {
    Log::logf(CAT_STORAGE,
              LOG_ERROR,
              "[DELETE] allocation failed context=%s bytes=%u\n",
              context ? context : "--",
              static_cast<unsigned>(bytes));
}

}  // namespace

struct StorageDeleteJob::WalkFrame {
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

void StorageDeleteJob::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    if (!published_status_.begin(status_)) {
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[DELETE] status snapshot unavailable\n");
    }
}

bool StorageDeleteJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageDeleteJob::unlock() const {
    if (status_dirty_ && published_status_.publish(status_)) {
        status_dirty_ = false;
    }
    if (lock_) xSemaphoreGive(lock_);
}

void StorageDeleteJob::touch_status_locked() {
    status_.updated_ms = nonzero_millis(millis());
    status_dirty_ = true;
}

void StorageDeleteJob::set_error_locked(const char *error) {
    release_walk_stack_locked();
    release_path_metadata_locked();
    status_.state = StorageDeleteState::Error;
    copy_cstr(status_.error, sizeof(status_.error), error);
    touch_status_locked();
    Log::logf(CAT_STORAGE, LOG_WARN, "[DELETE] error=%s\n",
              status_.error[0] ? status_.error : "error");
}

void StorageDeleteJob::reset_job_locked(bool keep_status) {
    release_walk_stack_locked();
    preempt_requested_.store(false);
    release_path_metadata_locked();

    if (!keep_status) {
        status_ = StorageDeleteStatus();
        status_.state = StorageDeleteState::Idle;
        touch_status_locked();
    }
}

void StorageDeleteJob::release_path_metadata_locked() {
    if (path_bytes_) {
        Memory::free(path_bytes_);
        path_bytes_ = nullptr;
    }
    path_bytes_len_ = 0;
    path_bytes_capacity_ = 0;
    current_root_ = 0;
    memset(root_offsets_, 0, sizeof(root_offsets_));
}

void StorageDeleteJob::close_walk_locked() {
    if (!walk_stack_) return;
    for (size_t i = 0; i < walk_depth_; ++i) {
        if (walk_stack_[i].opened) {
            walk_stack_[i].dir.close();
            walk_stack_[i].opened = false;
        }
    }
    walk_depth_ = 0;
}

bool StorageDeleteJob::ensure_walk_stack_locked() {
    if (walk_stack_) return true;
    walk_capacity_ = DELETE_MAX_DEPTH;
    walk_stack_ = static_cast<WalkFrame *>(
        Memory::alloc_large(sizeof(WalkFrame) * walk_capacity_, true));
    if (walk_stack_) {
        for (size_t i = 0; i < walk_capacity_; ++i) {
            new (&walk_stack_[i]) WalkFrame();
        }
        return true;
    }
    walk_capacity_ = 0;
    log_delete_alloc_failed("walk_stack",
                            sizeof(WalkFrame) * DELETE_MAX_DEPTH);
    return false;
}

void StorageDeleteJob::release_walk_stack_locked() {
    close_walk_locked();
    if (walk_stack_) {
        for (size_t i = 0; i < walk_capacity_; ++i) {
            walk_stack_[i].~WalkFrame();
        }
        Memory::free(walk_stack_);
    }
    walk_stack_ = nullptr;
    walk_capacity_ = 0;
}

void StorageDeleteJob::apply_preempt_locked() {
    if (!preempt_requested_.exchange(false)) return;
    if (status_.state == StorageDeleteState::Deleting) {
        close_walk_locked();
        touch_status_locked();
    }
}

bool StorageDeleteJob::reserve_path_bytes_locked(size_t needed) {
    if (needed <= path_bytes_capacity_) return true;
    size_t next = path_bytes_capacity_ ? path_bytes_capacity_ * 2
                                       : DELETE_INITIAL_PATH_BYTES;
    while (next < needed) next *= 2;
    char *bytes = static_cast<char *>(Memory::alloc_large(next, true));
    if (!bytes) {
        log_delete_alloc_failed("paths", next);
        return false;
    }
    if (path_bytes_) {
        memcpy(bytes, path_bytes_, path_bytes_len_);
        Memory::free(path_bytes_);
    }
    path_bytes_ = bytes;
    path_bytes_capacity_ = next;
    return true;
}

bool StorageDeleteJob::append_root_locked(const char *path) {
    if (!path || status_.roots >= AC_STORAGE_MAX_SELECTIONS) {
        set_error_locked("too_many_items");
        return false;
    }
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_PATH_MAX ||
        !reserve_path_bytes_locked(path_bytes_len_ + len + 1)) {
        set_error_locked("metadata_alloc");
        return false;
    }
    root_offsets_[status_.roots++] = static_cast<uint32_t>(path_bytes_len_);
    memcpy(path_bytes_ + path_bytes_len_, path, len + 1);
    path_bytes_len_ += len + 1;
    return true;
}

bool StorageDeleteJob::push_dir_locked(const char *path) {
    if (!walk_stack_ || walk_depth_ >= walk_capacity_) {
        set_error_locked("max_depth");
        return false;
    }
    WalkFrame &frame = walk_stack_[walk_depth_++];
    frame = WalkFrame();
    copy_cstr(frame.path, sizeof(frame.path), path);
    return true;
}

bool StorageDeleteJob::ensure_dir_open_locked(WalkFrame &frame) {
    if (frame.opened) return true;
    Storage::Guard guard;
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

bool StorageDeleteJob::start_selected(const char *base_path,
                                      const char *const *names,
                                      size_t count,
                                      uint32_t *id_out,
                                      char *error_out,
                                      size_t error_out_size) {
    if (id_out) *id_out = 0;
    copy_cstr(error_out, error_out_size, "");
    begin();
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
    if (status_.state == StorageDeleteState::Deleting) {
        copy_cstr(error_out, error_out_size, "delete_busy");
        unlock();
        return false;
    }
    reset_job_locked(false);
    if (!ensure_walk_stack_locked()) {
        set_error_locked("metadata_alloc");
        copy_cstr(error_out, error_out_size, status_.error);
        unlock();
        return false;
    }
    status_.state = StorageDeleteState::Deleting;
    status_.id = next_id_++;
    if (status_.id == 0) status_.id = next_id_++;
    copy_cstr(status_.base_path, sizeof(status_.base_path), normalized_base);
    status_.started_ms = nonzero_millis(millis());
    status_.updated_ms = status_.started_ms;
    status_dirty_ = true;

    bool base_ok = false;
    {
        Storage::Guard guard;
        File base_dir = Storage::open(status_.base_path, "r");
        base_ok = base_dir && base_dir.isDirectory();
        if (base_dir) base_dir.close();
    }
    if (!base_ok) {
        set_error_locked("not_directory");
        copy_cstr(error_out, error_out_size, status_.error);
        unlock();
        return false;
    }

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

    if (id_out) *id_out = status_.id;
    unlock();
    return true;
}

bool StorageDeleteJob::status(StorageDeleteStatus &out,
                              uint32_t timeout_ms) const {
    return published_status_.read(out, timeout_ms);
}

StorageDeleteStatus StorageDeleteJob::status() const {
    StorageDeleteStatus out;
    (void)status(out);
    return out;
}

bool StorageDeleteJob::active() const {
    if (!lock(20)) return true;
    const bool out = status_.state == StorageDeleteState::Deleting;
    unlock();
    return out;
}

JobStep StorageDeleteJob::step() {
    if (!lock(50)) return JobStep::Waiting;
    apply_preempt_locked();
    if (status_.state != StorageDeleteState::Deleting) {
        unlock();
        return JobStep::Idle;
    }
    if (!Storage::mounted()) {
        set_error_locked("storage_unavailable");
        unlock();
        return JobStep::Idle;
    }

    const uint32_t slice_started_us = micros();

    while (status_.state == StorageDeleteState::Deleting) {
        if (!delete_next_locked()) {
            unlock();
            return JobStep::Idle;
        }
        if (static_cast<uint32_t>(micros() - slice_started_us) >=
            DELETE_STEP_SLICE_US) {
            break;
        }
    }
    const JobStep result =
        status_.state == StorageDeleteState::Deleting ? JobStep::Working
                                                      : JobStep::Idle;
    unlock();
    return result;
}

void StorageDeleteJob::on_preempt() {
    preempt_requested_.store(true);
}

bool StorageDeleteJob::delete_next_locked() {
    if (walk_depth_ > 0) return delete_dir_step_locked();
    if (current_root_ >= status_.roots) return finish_done_locked();

    const char *path = path_bytes_ + root_offsets_[current_root_];
    bool exists = false;
    bool is_dir = false;
    {
        Storage::Guard guard;
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

bool StorageDeleteJob::delete_dir_step_locked() {
    WalkFrame &frame = walk_stack_[walk_depth_ - 1];
    if (!ensure_dir_open_locked(frame)) return false;

    StorageDirChild child;
    if (!storage_read_next_dir_child(frame.dir, child)) {
        {
            Storage::Guard guard;
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

bool StorageDeleteJob::finish_done_locked() {
    release_walk_stack_locked();
    release_path_metadata_locked();
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
