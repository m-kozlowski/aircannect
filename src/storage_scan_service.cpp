#include "storage_scan_service.h"

#include <new>
#include <string.h>
#include <utility>

#include <FS.h>

#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_internal.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t SCAN_STEP_BUDGET_MS = 3;
static constexpr size_t SCAN_MAX_DEPTH = 16;
static constexpr size_t SCAN_INITIAL_ENTRY_CAPACITY = 64;
static constexpr size_t SCAN_INITIAL_PATH_CAPACITY = 4096;

}  // namespace

struct StorageScanService::Job {
    struct Root {
        char path[AC_STORAGE_PATH_MAX] = {};
        bool recursive = false;
    };

    struct WalkFrame {
        char path[AC_STORAGE_PATH_MAX] = {};
        bool recursive = false;
        uint8_t root_index = 0;
        File directory;
    };

    bool active = false;
    bool abandoned = false;
    bool include_directories = false;
    OperationTicket ticket;
    Root roots[AC_STORAGE_SCAN_ROOT_MAX];
    size_t root_count = 0;
    size_t root_index = 0;
    WalkFrame walk[SCAN_MAX_DEPTH];
    size_t walk_depth = 0;
    std::shared_ptr<StorageScanSnapshot> snapshot;
};

StorageScanService::~StorageScanService() {
    if (job_) {
        close_directories_locked();
        job_->~Job();
        Memory::free(job_);
    }
    release_maintenance_locked();
    if (lock_) vSemaphoreDelete(lock_);
}

bool StorageScanService::begin(
    WakeCallback wake,
    ClaimMaintenanceCallback claim_maintenance,
    ReleaseMaintenanceCallback release_maintenance) {
    if (lock_) return job_ != nullptr;

    wake_ = wake;
    claim_maintenance_ = claim_maintenance;
    release_maintenance_ = release_maintenance;
    lock_ = xSemaphoreCreateMutex();

    void *memory = Memory::alloc_large(sizeof(Job), false);
    if (memory) job_ = new (memory) Job();
    if (lock_ && job_) return true;

    if (job_) {
        job_->~Job();
        Memory::free(job_);
        job_ = nullptr;
    }
    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
    return false;
}

void StorageScanService::set_task_available(bool available) {
    task_available_.store(available, std::memory_order_release);
    if (available) wake();
}

void StorageScanService::set_paused(bool paused) {
    paused_.store(paused, std::memory_order_release);
    if (!paused) wake();
}

bool StorageScanService::ready() const {
    return lock_ && job_ &&
           task_available_.load(std::memory_order_acquire);
}

bool StorageScanService::lock(uint32_t timeout_ms) const {
    return lock_ &&
           xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageScanService::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageScanService::wake() const {
    if (wake_) wake_();
}

bool StorageScanService::claim_maintenance_locked() {
    if (maintenance_claimed_) return true;
    if (!claim_maintenance_ || !claim_maintenance_()) return false;
    maintenance_claimed_ = true;
    return true;
}

void StorageScanService::release_maintenance_locked() {
    if (!maintenance_claimed_) return;
    if (release_maintenance_) release_maintenance_();
    maintenance_claimed_ = false;
}

OperationTicket StorageScanService::next_ticket_locked(
    uint32_t generation) {
    next_ticket_id_++;
    if (next_ticket_id_ == 0) next_ticket_id_++;
    return {next_ticket_id_, generation};
}

OperationSubmission StorageScanService::request_scan(
    const StorageScanCommand &command) {
    if (!command.valid()) return OperationSubmission::rejected();
    for (size_t i = 0; i < command.root_count; ++i) {
        if (!command.roots[i].path ||
            strlen(command.roots[i].path) >= AC_STORAGE_PATH_MAX ||
            !storage_user_path_valid(command.roots[i].path)) {
            return OperationSubmission::rejected();
        }
    }
    if (!ready()) return OperationSubmission::busy();
    if (!lock()) return OperationSubmission::busy();
    if (job_->active || completion_ready_) {
        unlock();
        return OperationSubmission::busy();
    }

    job_->active = true;
    job_->ticket = next_ticket_locked(command.generation);
    job_->include_directories = command.include_directories;
    job_->root_count = command.root_count;
    for (size_t i = 0; i < command.root_count; ++i) {
        copy_cstr(job_->roots[i].path,
                  sizeof(job_->roots[i].path),
                  command.roots[i].path);
        job_->roots[i].recursive = command.roots[i].recursive;
    }

    std::shared_ptr<StorageScanSnapshot> snapshot(
        new (std::nothrow) StorageScanSnapshot());
    if (!snapshot) {
        clear_job_locked();
        unlock();
        return OperationSubmission::busy();
    }
    snapshot->generation_ = command.generation;
    job_->snapshot = std::move(snapshot);

    const OperationTicket ticket = job_->ticket;
    unlock();
    wake();
    return OperationSubmission::accepted(ticket);
}

bool StorageScanService::abandon(OperationTicket ticket) {
    if (!ticket.valid() || !lock(50)) return false;

    if (job_->active && job_->ticket == ticket) {
        job_->abandoned = true;
        unlock();
        wake();
        return true;
    }
    if (completion_ready_ && completion_.ticket == ticket) {
        completion_ = {};
        completion_ready_ = false;
        unlock();
        return true;
    }

    unlock();
    return false;
}

bool StorageScanService::take_completion(
    OperationTicket ticket,
    StorageScanCompletion &completion) {
    if (!ticket.valid() || !lock()) return false;
    if (!completion_ready_ || completion_.ticket != ticket) {
        unlock();
        return false;
    }

    completion = std::move(completion_);
    completion_ = {};
    completion_ready_ = false;
    unlock();
    return true;
}

bool StorageScanService::reserve_entries_locked(size_t needed) {
    StorageScanSnapshot &snapshot = *job_->snapshot;
    if (needed <= snapshot.entry_capacity_) return true;

    size_t capacity = snapshot.entry_capacity_
        ? snapshot.entry_capacity_ * 2
        : SCAN_INITIAL_ENTRY_CAPACITY;
    while (capacity < needed) capacity *= 2;

    StorageScanSnapshot::Entry *entries =
        static_cast<StorageScanSnapshot::Entry *>(Memory::calloc_large(
            capacity, sizeof(StorageScanSnapshot::Entry), false));
    if (!entries) return false;

    if (snapshot.entries_) {
        memcpy(entries,
               snapshot.entries_,
               snapshot.entry_count_ * sizeof(*entries));
        Memory::free(snapshot.entries_);
    }
    snapshot.entries_ = entries;
    snapshot.entry_capacity_ = capacity;
    return true;
}

bool StorageScanService::reserve_paths_locked(size_t needed) {
    StorageScanSnapshot &snapshot = *job_->snapshot;
    if (needed <= snapshot.paths_capacity_) return true;

    size_t capacity = snapshot.paths_capacity_
        ? snapshot.paths_capacity_ * 2
        : SCAN_INITIAL_PATH_CAPACITY;
    while (capacity < needed) capacity *= 2;

    char *paths = static_cast<char *>(Memory::alloc_large(capacity, false));
    if (!paths) return false;

    if (snapshot.paths_) {
        memcpy(paths, snapshot.paths_, snapshot.paths_length_);
        Memory::free(snapshot.paths_);
    }
    snapshot.paths_ = paths;
    snapshot.paths_capacity_ = capacity;
    return true;
}

bool StorageScanService::append_entry_locked(const char *path,
                                             bool directory,
                                             uint8_t root_index,
                                             uint64_t size,
                                             uint64_t modified) {
    if (!job_->snapshot || !path || !path[0]) return false;

    StorageScanSnapshot &snapshot = *job_->snapshot;
    const size_t path_length = strlen(path) + 1;
    if (!reserve_entries_locked(snapshot.entry_count_ + 1) ||
        !reserve_paths_locked(snapshot.paths_length_ + path_length)) {
        return false;
    }

    StorageScanSnapshot::Entry &entry =
        snapshot.entries_[snapshot.entry_count_++];
    entry.path_offset = static_cast<uint32_t>(snapshot.paths_length_);
    entry.directory = directory;
    entry.root_index = root_index;
    entry.size = directory ? 0 : size;
    entry.modified = modified;

    memcpy(snapshot.paths_ + snapshot.paths_length_, path, path_length);
    snapshot.paths_length_ += path_length;
    return true;
}

bool StorageScanService::push_directory_locked(const char *path,
                                               bool recursive,
                                               uint8_t root_index,
                                               const char *&error) {
    if (job_->walk_depth >= SCAN_MAX_DEPTH) {
        error = "scan_depth_exceeded";
        return false;
    }

    Storage::Guard guard;
    File directory = Storage::open(path, "r");
    if (!directory || !directory.isDirectory()) {
        if (directory) directory.close();
        error = "scan_directory_open_failed";
        return false;
    }

    Job::WalkFrame &frame = job_->walk[job_->walk_depth++];
    copy_cstr(frame.path, sizeof(frame.path), path);
    frame.recursive = recursive;
    frame.root_index = root_index;
    frame.directory = directory;
    return true;
}

bool StorageScanService::start_next_root_locked(const char *&error) {
    while (job_->root_index < job_->root_count) {
        const size_t root_index = job_->root_index++;
        const Job::Root &root = job_->roots[root_index];

        bool exists = false;
        bool directory = false;
        uint64_t size = 0;
        time_t last_write = 0;
        {
            Storage::Guard guard;
            File node = Storage::open(root.path, "r");
            if (node) {
                exists = true;
                directory = node.isDirectory();
                size = directory ? 0 : node.size();
                last_write = node.getLastWrite();
                node.close();
            }
        }
        if (!exists) continue;

        const uint64_t modified = last_write > 0
            ? static_cast<uint64_t>(last_write)
            : 0;

        if (directory) {
            if (job_->include_directories &&
                !append_entry_locked(root.path,
                                     true,
                                     static_cast<uint8_t>(root_index),
                                     0,
                                     modified)) {
                error = "scan_snapshot_alloc_failed";
                return false;
            }
            return push_directory_locked(root.path,
                                         root.recursive,
                                         static_cast<uint8_t>(root_index),
                                         error);
        }

        if (!append_entry_locked(root.path,
                                 false,
                                 static_cast<uint8_t>(root_index),
                                 size,
                                 modified)) {
            error = "scan_snapshot_alloc_failed";
            return false;
        }
        return true;
    }
    return false;
}

bool StorageScanService::scan_next_entry_locked(const char *&error) {
    if (job_->walk_depth == 0) return start_next_root_locked(error);

    Job::WalkFrame &frame = job_->walk[job_->walk_depth - 1];
    StorageDirChild child;
    if (!storage_read_next_dir_child(frame.directory, child)) {
        {
            Storage::Guard guard;
            frame.directory.close();
        }
        frame = Job::WalkFrame();
        job_->walk_depth--;
        return true;
    }

    char child_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(frame.path,
                                   child.name,
                                   child_path,
                                   sizeof(child_path)) ||
        !storage_user_path_valid(child_path)) {
        error = "scan_child_path_invalid";
        return false;
    }

    const uint64_t modified = child.last_write > 0
        ? static_cast<uint64_t>(child.last_write)
        : 0;
    if (child.is_dir) {
        if (job_->include_directories &&
            !append_entry_locked(child_path,
                                 true,
                                 frame.root_index,
                                 0,
                                 modified)) {
            error = "scan_snapshot_alloc_failed";
            return false;
        }
        if (frame.recursive) {
            return push_directory_locked(child_path,
                                         true,
                                         frame.root_index,
                                         error);
        }
        return true;
    }

    if (!append_entry_locked(child_path,
                             false,
                             frame.root_index,
                             child.size,
                             modified)) {
        error = "scan_snapshot_alloc_failed";
        return false;
    }
    return true;
}

void StorageScanService::close_directories_locked() {
    if (!job_) return;
    while (job_->walk_depth > 0) {
        Job::WalkFrame &frame = job_->walk[job_->walk_depth - 1];
        if (frame.directory) {
            Storage::Guard guard;
            frame.directory.close();
        }
        frame = Job::WalkFrame();
        job_->walk_depth--;
    }
}

void StorageScanService::clear_job_locked() {
    if (!job_) return;
    close_directories_locked();
    *job_ = Job();
    release_maintenance_locked();
}

void StorageScanService::finish_locked(OperationOutcome outcome,
                                       const char *error) {
    if (!job_->abandoned) {
        completion_ = {};
        completion_.ticket = job_->ticket;
        completion_.outcome = outcome;
        completion_.snapshot = job_->snapshot;
        copy_cstr(completion_.error, sizeof(completion_.error), error);
        completion_ready_ = true;
    }
    clear_job_locked();
}

bool StorageScanService::step() {
    if (!ready() || paused_.load(std::memory_order_acquire) || !lock()) {
        return false;
    }
    if (!job_->active) {
        unlock();
        return false;
    }
    if (job_->abandoned) {
        clear_job_locked();
        unlock();
        return true;
    }
    if (!claim_maintenance_locked()) {
        unlock();
        return false;
    }
    if (!Storage::mounted()) {
        finish_locked(OperationOutcome::failed(), "storage_unavailable");
        unlock();
        return true;
    }

    const uint32_t started_ms = millis();
    bool worked = false;
    do {
        const char *error = nullptr;
        if (!scan_next_entry_locked(error)) {
            if (error) {
                finish_locked(OperationOutcome::failed(), error);
            } else {
                finish_locked(OperationOutcome::succeeded());
            }
            unlock();
            return true;
        }
        worked = true;
    } while (job_->active &&
             static_cast<uint32_t>(millis() - started_ms) <
                 SCAN_STEP_BUDGET_MS);

    unlock();
    return worked;
}

}  // namespace aircannect
