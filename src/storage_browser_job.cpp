#include "storage_browser_job.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>

#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_directory.h"
#include "storage_directory_order.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t LISTING_STEP_BUDGET_MS = 3;
static constexpr size_t LISTING_INITIAL_ENTRY_CAPACITY = 64;
static constexpr size_t LISTING_INITIAL_NAME_CAPACITY = 4096;

static constexpr size_t DOWNLOAD_RING_BYTES = 256 * 1024;
static constexpr size_t DOWNLOAD_READ_BYTES = 16 * 1024;
static constexpr size_t DOWNLOAD_READY_BYTES = 32 * 1024;
static constexpr uint32_t DOWNLOAD_ATTACH_TIMEOUT_MS = 60 * 1000;

void wake_background_worker() {
    if (BackgroundWorker *worker = background_worker()) worker->wake();
}

}  // namespace

struct StoragePreparedDownload {
    uint32_t id = 0;
    char path[AC_STORAGE_PATH_MAX] = {};
    char filename[AC_STORAGE_NAME_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};

    uint8_t *ring_storage = nullptr;
    PreparedByteRing ring;
    File input;
    uint64_t size = 0;
    uint64_t produced = 0;
    uint64_t consumed = 0;

    uint32_t ready_ms = 0;
    uint32_t consumer_activity_ms = 0;
    bool opening = false;
    bool metadata_ready = false;
    bool input_open = false;
    bool producer_done = false;
    bool consumer_attached = false;
    bool consumer_closed = false;
    std::atomic<bool> cancel_requested{false};

    ~StoragePreparedDownload() {
        if (ring_storage) Memory::free(ring_storage);
    }
};

class StorageDirectoryListing {
public:
    ~StorageDirectoryListing();

    void begin();
    StorageListingRead read(
        const char *path,
        bool refresh,
        std::shared_ptr<const StorageDirectorySnapshot> &snapshot_out,
        char *error_out,
        size_t error_out_size);
    JobStep step();
    void on_preempt();

private:
    enum class Phase : uint8_t {
        Idle,
        Scanning,
        Sorting,
    };

    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;

    bool start_pending_locked();
    bool ensure_dir_open_locked();
    bool append_entry_locked(const char *name, bool directory, uint64_t size,
                             uint64_t modified);
    bool reserve_entries_locked(size_t needed);
    bool reserve_names_locked(size_t needed);
    JobStep step_locked();
    JobStep sort_step_locked();
    void publish_locked();
    void fail_locked(const char *error);
    void close_dir_locked();
    void clear_build_locked();
    int find_snapshot_locked(const char *path) const;
    void touch_snapshot_locked(size_t index);

    mutable SemaphoreHandle_t lock_ = nullptr;

    static constexpr size_t SNAPSHOT_SLOTS = 2;
    std::shared_ptr<const StorageDirectorySnapshot> snapshots_[SNAPSHOT_SLOTS];
    uint32_t snapshot_touches_[SNAPSHOT_SLOTS] = {};
    uint32_t snapshot_touch_counter_ = 0;
    uint32_t next_snapshot_revision_ = 1;

    Phase phase_ = Phase::Idle;
    std::shared_ptr<StorageDirectorySnapshot> build_;
    File dir_;
    bool dir_open_ = false;
    uint32_t scanned_ = 0;

    size_t sort_width_ = 0;
    size_t sort_left_ = 0;
    size_t sort_mid_ = 0;
    size_t sort_right_ = 0;
    size_t sort_source_left_ = 0;
    size_t sort_source_right_ = 0;
    size_t sort_destination_ = 0;
    bool sort_source_is_entries_ = true;
    bool sort_merge_active_ = false;

    char pending_path_[AC_STORAGE_PATH_MAX] = {};
    bool pending_ = false;
    char error_path_[AC_STORAGE_PATH_MAX] = {};
    char error_[AC_STORAGE_ERROR_MAX] = {};
};

class StorageDownloadProducer {
public:
    ~StorageDownloadProducer();

    void begin();
    bool active_or_busy() const;
    StorageDownloadPrepareState prepare(
        const char *path,
        StorageDownloadPrepareStatus &status_out);
    bool attach(uint32_t id,
                std::shared_ptr<StoragePreparedDownload> &download_out,
                char *filename_out,
                size_t filename_out_size,
                uint64_t &size_out,
                char *error_out,
                size_t error_out_size);
    PreparedByteRead read(StoragePreparedDownload &download,
                          uint8_t *buffer,
                          size_t max_length,
                          size_t offset);
    void finish(StoragePreparedDownload &download);
    JobStep step();
    void on_preempt();

private:
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void fail_locked(StoragePreparedDownload &download, const char *error);
    void close_file(StoragePreparedDownload &download);
    void retire_locked(StoragePreparedDownload &download, bool success);

    mutable SemaphoreHandle_t lock_ = nullptr;
    std::shared_ptr<StoragePreparedDownload> active_;
    uint32_t next_id_ = 1;
};

StorageDirectorySnapshot::~StorageDirectorySnapshot() {
    if (entries_) Memory::free(entries_);
    if (sort_entries_) Memory::free(sort_entries_);
    if (names_) Memory::free(names_);
}

bool StorageDirectorySnapshot::entry(size_t index,
                                     StorageDirectoryEntryView &out) const {
    out = StorageDirectoryEntryView();
    if (index >= entry_count_ || !entries_ || !names_) return false;

    const Entry &entry = entries_[index];
    if (entry.name_offset >= names_length_) return false;

    out.name = names_ + entry.name_offset;
    out.directory = entry.directory;
    out.size = entry.size;
    out.modified = entry.modified;
    return true;
}

StorageDirectoryListing::~StorageDirectoryListing() {
    on_preempt();
    if (lock_) vSemaphoreDelete(lock_);
}

void StorageDirectoryListing::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
}

bool StorageDirectoryListing::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageDirectoryListing::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

StorageDownloadProducer::~StorageDownloadProducer() {
    on_preempt();
    if (lock_) vSemaphoreDelete(lock_);
}

void StorageDownloadProducer::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
}

bool StorageDownloadProducer::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageDownloadProducer::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

int StorageDirectoryListing::find_snapshot_locked(const char *path) const {
    for (size_t i = 0; i < SNAPSHOT_SLOTS; ++i) {
        if (snapshots_[i] && strcmp(snapshots_[i]->path(), path) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void StorageDirectoryListing::touch_snapshot_locked(size_t index) {
    if (index >= SNAPSHOT_SLOTS) return;
    snapshot_touch_counter_++;
    if (snapshot_touch_counter_ == 0) snapshot_touch_counter_ = 1;
    snapshot_touches_[index] = snapshot_touch_counter_;
}

StorageListingRead StorageDirectoryListing::read(
    const char *path,
    bool refresh,
    std::shared_ptr<const StorageDirectorySnapshot> &snapshot_out,
    char *error_out,
    size_t error_out_size) {
    snapshot_out.reset();
    copy_cstr(error_out, error_out_size, "");
    begin();

    if (!path || !storage_user_path_valid(path)) {
        copy_cstr(error_out, error_out_size, "bad_path");
        return StorageListingRead::Error;
    }
    if (!lock(0)) return StorageListingRead::Preparing;

    const bool active_same = build_ &&
        strcmp(build_->path(), path) == 0;
    const bool pending_same = pending_ &&
        strcmp(pending_path_, path) == 0;

    if (refresh && !active_same && !pending_same) {
        copy_cstr(pending_path_, sizeof(pending_path_), path);
        pending_ = true;
        error_path_[0] = 0;
        error_[0] = 0;
    }

    if (active_same || pending_same || refresh) {
        unlock();
        wake_background_worker();
        return StorageListingRead::Preparing;
    }

    const int ready = find_snapshot_locked(path);
    if (ready >= 0) {
        touch_snapshot_locked(static_cast<size_t>(ready));
        snapshot_out = snapshots_[ready];
        unlock();
        return StorageListingRead::Ready;
    }

    if (strcmp(error_path_, path) == 0 && error_[0]) {
        copy_cstr(error_out, error_out_size, error_);
        unlock();
        return StorageListingRead::Error;
    }

    copy_cstr(pending_path_, sizeof(pending_path_), path);
    pending_ = true;
    error_path_[0] = 0;
    error_[0] = 0;
    unlock();

    wake_background_worker();
    return StorageListingRead::Preparing;
}

void StorageDirectoryListing::close_dir_locked() {
    if (!dir_open_) return;

    Storage::Guard guard;
    dir_.close();
    dir_open_ = false;
}

void StorageDirectoryListing::clear_build_locked() {
    close_dir_locked();
    build_.reset();
    phase_ = Phase::Idle;
    scanned_ = 0;
    sort_width_ = 0;
    sort_left_ = 0;
    sort_mid_ = 0;
    sort_right_ = 0;
    sort_source_left_ = 0;
    sort_source_right_ = 0;
    sort_destination_ = 0;
    sort_source_is_entries_ = true;
    sort_merge_active_ = false;
}

bool StorageDirectoryListing::start_pending_locked() {
    if (!pending_) return false;

    clear_build_locked();
    std::shared_ptr<StorageDirectorySnapshot> snapshot(
        new (std::nothrow) StorageDirectorySnapshot());
    if (!snapshot) {
        copy_cstr(error_path_, sizeof(error_path_),
                  pending_path_);
        copy_cstr(error_, sizeof(error_), "snapshot_alloc");
        pending_ = false;
        return false;
    }

    copy_cstr(snapshot->path_, sizeof(snapshot->path_), pending_path_);
    build_ = snapshot;
    phase_ = Phase::Scanning;
    scanned_ = 0;
    pending_ = false;
    return true;
}

bool StorageDirectoryListing::ensure_dir_open_locked() {
    if (dir_open_) return true;
    if (!build_) return false;

    Storage::Guard guard;
    dir_ = Storage::open(build_->path(), "r");
    if (!dir_) {
        fail_locked("not_found");
        return false;
    }
    if (!dir_.isDirectory()) {
        dir_.close();
        fail_locked("not_directory");
        return false;
    }
    if (scanned_ > 0 &&
        !storage_skip_dir_children(dir_, scanned_)) {
        dir_.close();
        fail_locked("scan_resume_failed");
        return false;
    }

    dir_open_ = true;
    return true;
}

bool StorageDirectoryListing::reserve_entries_locked(size_t needed) {
    if (!build_ || needed <= build_->entry_capacity_) return true;

    size_t capacity = build_->entry_capacity_
        ? build_->entry_capacity_ * 2
        : LISTING_INITIAL_ENTRY_CAPACITY;
    while (capacity < needed) capacity *= 2;

    StorageDirectorySnapshot::Entry *entries =
        static_cast<StorageDirectorySnapshot::Entry *>(Memory::calloc_large(
            capacity, sizeof(StorageDirectorySnapshot::Entry), false));
    if (!entries) return false;

    if (build_->entries_) {
        memcpy(entries,
               build_->entries_,
               build_->entry_count_ * sizeof(*entries));
        Memory::free(build_->entries_);
    }
    build_->entries_ = entries;
    build_->entry_capacity_ = capacity;
    return true;
}

bool StorageDirectoryListing::reserve_names_locked(size_t needed) {
    if (!build_ || needed <= build_->names_capacity_) return true;

    size_t capacity = build_->names_capacity_
        ? build_->names_capacity_ * 2
        : LISTING_INITIAL_NAME_CAPACITY;
    while (capacity < needed) capacity *= 2;

    char *names = static_cast<char *>(Memory::alloc_large(capacity, false));
    if (!names) return false;

    if (build_->names_) {
        memcpy(names, build_->names_, build_->names_length_);
        Memory::free(build_->names_);
    }
    build_->names_ = names;
    build_->names_capacity_ = capacity;
    return true;
}

bool StorageDirectoryListing::append_entry_locked(const char *name,
                                                   bool directory,
                                                   uint64_t size,
                                                   uint64_t modified) {
    if (!build_ || !name || !*name) return false;

    const size_t name_length = strlen(name) + 1;
    if (!reserve_entries_locked(build_->entry_count_ + 1) ||
        !reserve_names_locked(build_->names_length_ + name_length)) {
        return false;
    }

    StorageDirectorySnapshot::Entry &entry =
        build_->entries_[build_->entry_count_++];
    entry.name_offset = static_cast<uint32_t>(build_->names_length_);
    entry.directory = directory;
    entry.size = directory ? 0 : size;
    entry.modified = modified;

    memcpy(build_->names_ + build_->names_length_,
           name,
           name_length);
    build_->names_length_ += name_length;
    return true;
}

void StorageDirectoryListing::fail_locked(const char *error) {
    if (build_) {
        copy_cstr(error_path_, sizeof(error_path_),
                  build_->path());
    }
    copy_cstr(error_, sizeof(error_), error);
    clear_build_locked();
}

void StorageDirectoryListing::publish_locked() {
    if (!build_) return;

    build_->revision_ = next_snapshot_revision_++;
    if (build_->revision_ == 0) {
        build_->revision_ = next_snapshot_revision_++;
    }

    int slot = find_snapshot_locked(build_->path());
    if (slot < 0) {
        for (size_t i = 0; i < SNAPSHOT_SLOTS; ++i) {
            if (!snapshots_[i]) {
                slot = static_cast<int>(i);
                break;
            }
        }
    }
    if (slot < 0) {
        slot = snapshot_touches_[0] <= snapshot_touches_[1] ? 0 : 1;
    }

    snapshots_[slot] = build_;
    touch_snapshot_locked(static_cast<size_t>(slot));
    error_path_[0] = 0;
    error_[0] = 0;
    clear_build_locked();
}

JobStep StorageDirectoryListing::sort_step_locked() {
    if (!build_) return JobStep::Idle;

    const size_t count = build_->entry_count_;
    if (count < 2) {
        publish_locked();
        return JobStep::Working;
    }

    const uint32_t started_ms = millis();
    while (build_ &&
           static_cast<uint32_t>(millis() - started_ms) <
               LISTING_STEP_BUDGET_MS) {
        if (!sort_merge_active_ && sort_left_ >= count) {
            sort_source_is_entries_ = !sort_source_is_entries_;
            sort_width_ *= 2;
            sort_left_ = 0;

            if (sort_width_ >= count) {
                if (!sort_source_is_entries_) {
                    std::swap(build_->entries_,
                              build_->sort_entries_);
                    build_->entry_capacity_ = count;
                }
                Memory::free(build_->sort_entries_);
                build_->sort_entries_ = nullptr;
                publish_locked();
                return JobStep::Working;
            }
        }

        if (!sort_merge_active_) {
            sort_mid_ = std::min(sort_left_ + sort_width_, count);
            sort_right_ = std::min(sort_left_ + sort_width_ * 2, count);
            sort_source_left_ = sort_left_;
            sort_source_right_ = sort_mid_;
            sort_destination_ = sort_left_;
            sort_merge_active_ = true;
        }

        StorageDirectorySnapshot::Entry *source = sort_source_is_entries_
            ? build_->entries_
            : build_->sort_entries_;
        StorageDirectorySnapshot::Entry *destination = sort_source_is_entries_
            ? build_->sort_entries_
            : build_->entries_;

        while (sort_merge_active_ &&
               static_cast<uint32_t>(millis() - started_ms) <
                   LISTING_STEP_BUDGET_MS) {
            bool take_left = sort_source_right_ >= sort_right_;
            if (sort_source_left_ < sort_mid_ &&
                sort_source_right_ < sort_right_) {
                const StorageDirectorySnapshot::Entry &left =
                    source[sort_source_left_];
                const StorageDirectorySnapshot::Entry &right =
                    source[sort_source_right_];
                take_left = storage_directory_entry_order(
                    left.modified,
                    build_->names_ + left.name_offset,
                    right.modified,
                    build_->names_ + right.name_offset) <= 0;
            }

            if (take_left && sort_source_left_ < sort_mid_) {
                destination[sort_destination_++] = source[sort_source_left_++];
            } else if (sort_source_right_ < sort_right_) {
                destination[sort_destination_++] = source[sort_source_right_++];
            }

            if (sort_destination_ >= sort_right_) {
                sort_left_ = sort_right_;
                sort_merge_active_ = false;
            }
        }
    }
    return JobStep::Working;
}

JobStep StorageDirectoryListing::step_locked() {
    if (pending_ &&
        (!build_ ||
         strcmp(build_->path(), pending_path_) != 0)) {
        (void)start_pending_locked();
    } else if (!build_) {
        (void)start_pending_locked();
    }
    if (!build_) return JobStep::Idle;

    if (phase_ == Phase::Sorting) {
        return sort_step_locked();
    }
    if (!Storage::mounted()) {
        fail_locked("storage_unavailable");
        return JobStep::Idle;
    }
    if (!ensure_dir_open_locked()) return JobStep::Idle;

    const uint32_t started_ms = millis();
    while (build_ &&
           static_cast<uint32_t>(millis() - started_ms) <
               LISTING_STEP_BUDGET_MS) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir_, child)) {
            close_dir_locked();
            if (build_->entry_count_ < 2) {
                publish_locked();
                return JobStep::Working;
            }

            build_->sort_entries_ =
                static_cast<StorageDirectorySnapshot::Entry *>(
                    Memory::calloc_large(
                        build_->entry_count_,
                        sizeof(StorageDirectorySnapshot::Entry),
                        false));
            if (!build_->sort_entries_) {
                fail_locked("sort_alloc");
                return JobStep::Idle;
            }

            phase_ = Phase::Sorting;
            sort_width_ = 1;
            sort_left_ = 0;
            sort_source_is_entries_ = true;
            sort_merge_active_ = false;
            return JobStep::Working;
        }
        scanned_++;

        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(build_->path(),
                                       child.name,
                                       child_path,
                                       sizeof(child_path)) ||
            !storage_user_path_valid(child_path)) {
            continue;
        }

        const uint64_t modified = child.last_write > 0
            ? static_cast<uint64_t>(child.last_write)
            : 0;
        if (!append_entry_locked(child.name,
                                 child.is_dir,
                                 child.size,
                                 modified)) {
            fail_locked("snapshot_alloc");
            return JobStep::Idle;
        }
    }
    return JobStep::Working;
}

StorageDownloadPrepareState StorageDownloadProducer::prepare(
    const char *path,
    StorageDownloadPrepareStatus &status_out) {
    status_out = StorageDownloadPrepareStatus();
    begin();

    if (!path || !storage_user_path_valid(path)) {
        status_out.state = StorageDownloadPrepareState::Error;
        copy_cstr(status_out.error, sizeof(status_out.error), "bad_path");
        return status_out.state;
    }
    if (!lock(0)) return status_out.state;

    if (active_) {
        StoragePreparedDownload &active = *active_;
        if (active.error[0] && strcmp(active.path, path) == 0) {
            status_out.state = StorageDownloadPrepareState::Error;
            status_out.id = active.id;
            copy_cstr(status_out.error, sizeof(status_out.error), active.error);
            active_.reset();
            unlock();
            return status_out.state;
        }
        if (strcmp(active.path, path) != 0) {
            status_out.state = StorageDownloadPrepareState::Busy;
            copy_cstr(status_out.error, sizeof(status_out.error),
                      "download_busy");
            unlock();
            return status_out.state;
        }

        status_out.id = active.id;
        status_out.size = active.size;
        copy_cstr(status_out.filename, sizeof(status_out.filename),
                  active.filename);
        const bool ready = active.metadata_ready &&
            (active.ring.readable() >= DOWNLOAD_READY_BYTES ||
             active.producer_done);
        status_out.state = ready ? StorageDownloadPrepareState::Ready
                                 : StorageDownloadPrepareState::Preparing;
        unlock();
        wake_background_worker();
        return status_out.state;
    }

    std::shared_ptr<StoragePreparedDownload> download(
        new (std::nothrow) StoragePreparedDownload());
    if (!download) {
        status_out.state = StorageDownloadPrepareState::Error;
        copy_cstr(status_out.error, sizeof(status_out.error),
                  "download_alloc");
        unlock();
        return status_out.state;
    }

    download->id = next_id_++;
    if (download->id == 0) download->id = next_id_++;
    copy_cstr(download->path, sizeof(download->path), path);
    copy_cstr(download->filename, sizeof(download->filename),
              storage_basename_from_path(path));
    active_ = download;
    status_out.id = download->id;
    unlock();

    wake_background_worker();
    return status_out.state;
}

bool StorageDownloadProducer::attach(
    uint32_t id,
    std::shared_ptr<StoragePreparedDownload> &download_out,
    char *filename_out,
    size_t filename_out_size,
    uint64_t &size_out,
    char *error_out,
    size_t error_out_size) {
    download_out.reset();
    size_out = 0;
    copy_cstr(error_out, error_out_size, "");
    begin();

    if (!lock(0)) {
        copy_cstr(error_out, error_out_size, "not_ready");
        return false;
    }
    if (!active_ || active_->id != id ||
        active_->error[0] ||
        !active_->metadata_ready ||
        (active_->ring.readable() < DOWNLOAD_READY_BYTES &&
         !active_->producer_done) ||
        active_->consumer_attached) {
        copy_cstr(error_out, error_out_size, "not_ready");
        unlock();
        return false;
    }

    active_->consumer_attached = true;
    active_->consumer_activity_ms = nonzero_millis(millis());
    copy_cstr(filename_out, filename_out_size, active_->filename);
    size_out = active_->size;
    download_out = active_;
    unlock();

    wake_background_worker();
    return true;
}

PreparedByteRead StorageDownloadProducer::read(
    StoragePreparedDownload &download,
    uint8_t *buffer,
    size_t max_length,
    size_t offset) {
    PreparedByteRead result;
    if (!buffer || max_length == 0 || !lock(0)) {
        result.state = PreparedByteReadState::Retry;
        return result;
    }

    if (!active_ || active_.get() != &download ||
        download.error[0] || offset != download.consumed) {
        unlock();
        return result;
    }

    result.bytes = download.ring.read(buffer, max_length);
    if (result.bytes > 0) {
        download.consumed += result.bytes;
        download.consumer_activity_ms = nonzero_millis(millis());
        result.state = PreparedByteReadState::Data;
    } else if (download.producer_done) {
        result.state = PreparedByteReadState::End;
    } else {
        result.state = PreparedByteReadState::Retry;
    }
    unlock();

    if (result.bytes > 0) wake_background_worker();
    return result;
}

void StorageDownloadProducer::finish(StoragePreparedDownload &download) {
    if (!lock(0)) {
        download.cancel_requested.store(true, std::memory_order_relaxed);
        wake_background_worker();
        return;
    }
    if (active_.get() == &download) {
        download.consumer_closed = true;
        if (download.consumed < download.size) {
            download.cancel_requested.store(true, std::memory_order_relaxed);
        }
    }
    unlock();
    wake_background_worker();
}

void StorageDownloadProducer::close_file(StoragePreparedDownload &download) {
    if (!download.input_open) return;

    Storage::Guard guard;
    download.input.close();
    download.input_open = false;
}

void StorageDownloadProducer::fail_locked(StoragePreparedDownload &download,
                                          const char *error) {
    copy_cstr(download.error, sizeof(download.error), error);
    if (download.ready_ms == 0) download.ready_ms = nonzero_millis(millis());
    download.producer_done = true;
}

void StorageDownloadProducer::retire_locked(StoragePreparedDownload &download,
                                            bool success) {
    if (!success && !download.error[0]) {
        copy_cstr(download.error, sizeof(download.error), "download_aborted");
    }
    active_.reset();
}

JobStep StorageDownloadProducer::step() {
    if (!lock(20)) return JobStep::Waiting;
    std::shared_ptr<StoragePreparedDownload> download = active_;
    if (!download) {
        unlock();
        return JobStep::Idle;
    }

    if (download->cancel_requested.load(std::memory_order_relaxed)) {
        unlock();
        close_file(*download);
        if (!lock(20)) return JobStep::Waiting;
        if (active_.get() == download.get()) {
            retire_locked(*download, false);
        }
        unlock();
        return JobStep::Idle;
    }

    if (download->producer_done) {
        const uint32_t now_ms = nonzero_millis(millis());
        const bool attach_expired = !download->consumer_attached &&
            millis_deadline_reached(
                now_ms, download->ready_ms + DOWNLOAD_ATTACH_TIMEOUT_MS);

        if (attach_expired) {
            retire_locked(*download, !download->error[0]);
            unlock();
            return JobStep::Idle;
        }

        const bool complete = download->consumed == download->size;
        const bool retire = download->consumer_closed &&
            (complete || download->error[0]);
        if (retire) retire_locked(*download, complete);
        unlock();
        return JobStep::Waiting;
    }

    if (!download->metadata_ready && !download->opening) {
        download->opening = true;

        uint8_t *ring_storage = static_cast<uint8_t *>(
            Memory::alloc_large(DOWNLOAD_RING_BYTES, false));
        File input;
        bool directory = false;
        uint64_t size = 0;
        if (ring_storage) {
            Storage::Guard guard;
            input = Storage::open(download->path, "r");
            if (input) {
                directory = input.isDirectory();
                if (!directory) {
                    size = static_cast<uint64_t>(input.size());
                    (void)input.setBufferSize(512);
                }
            }
        }

        download->opening = false;
        if (!ring_storage) {
            fail_locked(*download, "buffer_alloc");
        } else if (!input || directory) {
            Memory::free(ring_storage);
            if (input) {
                Storage::Guard guard;
                input.close();
            }
            fail_locked(*download,
                        directory ? "not_file" : "not_found");
        } else {
            download->ring_storage = ring_storage;
            download->ring.bind(ring_storage, DOWNLOAD_RING_BYTES);
            download->input = input;
            download->input_open = true;
            download->size = size;
            download->metadata_ready = true;
            download->ready_ms = nonzero_millis(millis());
            if (size == 0) {
                download->producer_done = true;
            }
        }
        const bool failed = download->error[0] != 0;
        unlock();

        if (download->producer_done) close_file(*download);
        return failed ? JobStep::Idle : JobStep::Working;
    }

    const uint32_t now_ms = nonzero_millis(millis());
    if (!download->consumer_attached && millis_deadline_reached(
            now_ms, download->ready_ms + DOWNLOAD_ATTACH_TIMEOUT_MS)) {
        download->cancel_requested.store(true, std::memory_order_relaxed);
        unlock();
        return JobStep::Working;
    }
    if (download->consumer_attached && !download->consumer_closed &&
        millis_deadline_reached(
            now_ms,
            download->consumer_activity_ms + DOWNLOAD_ATTACH_TIMEOUT_MS)) {
        download->cancel_requested.store(true, std::memory_order_relaxed);
        unlock();
        return JobStep::Working;
    }

    size_t span_length = 0;
    uint8_t *span = download->ring.write_span(span_length);
    if (!span || span_length == 0) {
        unlock();
        return JobStep::Waiting;
    }
    span_length = std::min(span_length, DOWNLOAD_READ_BYTES);
    const uint64_t remaining = download->size - download->produced;
    const size_t wanted = remaining < span_length
        ? static_cast<size_t>(remaining)
        : span_length;

    size_t bytes_read = 0;
    if (wanted > 0) {
        Storage::Guard guard;
        bytes_read = download->input.read(span, wanted);
    }

    if (wanted > 0 && bytes_read == 0) {
        fail_locked(*download, "read_failed");
        unlock();
        close_file(*download);
        return JobStep::Idle;
    }
    if (!download->ring.commit_write(bytes_read)) {
        fail_locked(*download, "ring_commit");
        unlock();
        close_file(*download);
        return JobStep::Idle;
    }

    download->produced += bytes_read;
    if (download->produced >= download->size) {
        download->producer_done = true;
    }
    const bool done = download->producer_done;
    unlock();

    if (done) close_file(*download);
    return JobStep::Working;
}

JobStep StorageDirectoryListing::step() {
    begin();
    if (!lock(20)) return JobStep::Waiting;

    const JobStep result = step_locked();
    unlock();
    return result;
}

void StorageDirectoryListing::on_preempt() {
    begin();
    if (!lock(20)) return;

    close_dir_locked();
    unlock();
}

bool StorageDownloadProducer::active_or_busy() const {
    // Lock contention means a foreground download operation is in flight.
    if (!lock(0)) return true;

    const bool result = static_cast<bool>(active_);
    unlock();
    return result;
}

void StorageDownloadProducer::on_preempt() {
    begin();
    if (!lock(20)) return;

    std::shared_ptr<StoragePreparedDownload> download = active_;
    if (download) {
        download->cancel_requested.store(true, std::memory_order_relaxed);
    }
    unlock();

    if (!download) return;
    close_file(*download);
    if (!lock(20)) return;
    if (active_.get() == download.get()) {
        fail_locked(*download, "preempted");
        if (!download->consumer_attached || download->consumer_closed) {
            active_.reset();
        }
    }
    unlock();
}

StorageBrowserJob::~StorageBrowserJob() {
    if (download_) {
        download_->~StorageDownloadProducer();
        Memory::free(download_);
    }
    if (listing_) {
        listing_->~StorageDirectoryListing();
        Memory::free(listing_);
    }
}

bool StorageBrowserJob::ensure_owners() {
    if (!listing_) {
        void *memory = Memory::alloc_large(sizeof(StorageDirectoryListing),
                                           false);
        if (!memory) return false;
        listing_ = new (memory) StorageDirectoryListing();
    }
    if (!download_) {
        void *memory = Memory::alloc_large(sizeof(StorageDownloadProducer),
                                           false);
        if (!memory) return false;
        download_ = new (memory) StorageDownloadProducer();
    }

    listing_->begin();
    download_->begin();
    return true;
}

void StorageBrowserJob::begin() {
    if (!ensure_owners()) {
        Log::logf(CAT_STORAGE,
                  LOG_ERROR,
                  "browser owner allocation failed\n");
    }
}

bool StorageBrowserJob::run_when_gate_closed(const char *reason) const {
    return reason && strcmp(reason, "web_grace") == 0;
}

StorageListingRead StorageBrowserJob::listing(
    const char *path,
    bool refresh,
    std::shared_ptr<const StorageDirectorySnapshot> &snapshot_out,
    char *error_out,
    size_t error_out_size) {
    if (!ensure_owners()) {
        copy_cstr(error_out, error_out_size, "owner_alloc");
        return StorageListingRead::Error;
    }
    return listing_->read(path,
                          refresh,
                          snapshot_out,
                          error_out,
                          error_out_size);
}

StorageDownloadPrepareState StorageBrowserJob::prepare_download(
    const char *path,
    StorageDownloadPrepareStatus &status_out) {
    if (!ensure_owners()) {
        status_out = StorageDownloadPrepareStatus();
        status_out.state = StorageDownloadPrepareState::Error;
        copy_cstr(status_out.error, sizeof(status_out.error), "owner_alloc");
        return status_out.state;
    }
    return download_->prepare(path, status_out);
}

bool StorageBrowserJob::begin_download(
    uint32_t id,
    std::shared_ptr<StoragePreparedDownload> &download_out,
    char *filename_out,
    size_t filename_out_size,
    uint64_t &size_out,
    char *error_out,
    size_t error_out_size) {
    if (!ensure_owners()) {
        copy_cstr(error_out, error_out_size, "owner_alloc");
        return false;
    }
    return download_->attach(id,
                             download_out,
                             filename_out,
                             filename_out_size,
                             size_out,
                             error_out,
                             error_out_size);
}

PreparedByteRead StorageBrowserJob::read_download(
    StoragePreparedDownload &download,
    uint8_t *buffer,
    size_t max_length,
    size_t offset) {
    if (!download_) return PreparedByteRead();
    return download_->read(download, buffer, max_length, offset);
}

void StorageBrowserJob::finish_download(StoragePreparedDownload &download) {
    if (download_) download_->finish(download);
}

JobStep StorageBrowserJob::step() {
    if (!ensure_owners()) return JobStep::Idle;
    return download_->active_or_busy() ? download_->step() : listing_->step();
}

void StorageBrowserJob::on_preempt() {
    if (listing_) listing_->on_preempt();
    if (download_) download_->on_preempt();
}

}  // namespace aircannect
