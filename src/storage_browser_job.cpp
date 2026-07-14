#include "storage_browser_job.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>

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

void StorageBrowserJob::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutexStatic(&lock_storage_);
}

bool StorageBrowserJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageBrowserJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

bool StorageBrowserJob::run_when_gate_closed(const char *reason) const {
    return reason && strcmp(reason, "web_grace") == 0;
}

int StorageBrowserJob::find_snapshot_locked(const char *path) const {
    for (size_t i = 0; i < SNAPSHOT_SLOTS; ++i) {
        if (snapshots_[i] && strcmp(snapshots_[i]->path(), path) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void StorageBrowserJob::touch_snapshot_locked(size_t index) {
    if (index >= SNAPSHOT_SLOTS) return;
    snapshot_touch_counter_++;
    if (snapshot_touch_counter_ == 0) snapshot_touch_counter_ = 1;
    snapshot_touches_[index] = snapshot_touch_counter_;
}

StorageListingRead StorageBrowserJob::listing(
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

    const bool active_same = listing_build_ &&
        strcmp(listing_build_->path(), path) == 0;
    const bool pending_same = pending_listing_ &&
        strcmp(pending_listing_path_, path) == 0;

    if (refresh && !active_same && !pending_same) {
        copy_cstr(pending_listing_path_, sizeof(pending_listing_path_), path);
        pending_listing_ = true;
        listing_error_path_[0] = 0;
        listing_error_[0] = 0;
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

    if (strcmp(listing_error_path_, path) == 0 && listing_error_[0]) {
        copy_cstr(error_out, error_out_size, listing_error_);
        unlock();
        return StorageListingRead::Error;
    }

    copy_cstr(pending_listing_path_, sizeof(pending_listing_path_), path);
    pending_listing_ = true;
    listing_error_path_[0] = 0;
    listing_error_[0] = 0;
    unlock();

    wake_background_worker();
    return StorageListingRead::Preparing;
}

void StorageBrowserJob::close_listing_dir_locked() {
    if (!listing_dir_open_) return;

    Storage::Guard guard;
    listing_dir_.close();
    listing_dir_open_ = false;
}

void StorageBrowserJob::clear_listing_build_locked() {
    close_listing_dir_locked();
    listing_build_.reset();
    listing_phase_ = ListingPhase::Idle;
    listing_scanned_ = 0;
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

bool StorageBrowserJob::start_pending_listing_locked() {
    if (!pending_listing_) return false;

    clear_listing_build_locked();
    std::shared_ptr<StorageDirectorySnapshot> snapshot(
        new (std::nothrow) StorageDirectorySnapshot());
    if (!snapshot) {
        copy_cstr(listing_error_path_, sizeof(listing_error_path_),
                  pending_listing_path_);
        copy_cstr(listing_error_, sizeof(listing_error_), "snapshot_alloc");
        pending_listing_ = false;
        return false;
    }

    copy_cstr(snapshot->path_, sizeof(snapshot->path_), pending_listing_path_);
    listing_build_ = snapshot;
    listing_phase_ = ListingPhase::Scanning;
    listing_scanned_ = 0;
    pending_listing_ = false;
    return true;
}

bool StorageBrowserJob::ensure_listing_dir_open_locked() {
    if (listing_dir_open_) return true;
    if (!listing_build_) return false;

    Storage::Guard guard;
    listing_dir_ = Storage::open(listing_build_->path(), "r");
    if (!listing_dir_) {
        fail_listing_locked("not_found");
        return false;
    }
    if (!listing_dir_.isDirectory()) {
        listing_dir_.close();
        fail_listing_locked("not_directory");
        return false;
    }
    if (listing_scanned_ > 0 &&
        !storage_skip_dir_children(listing_dir_, listing_scanned_)) {
        listing_dir_.close();
        fail_listing_locked("scan_resume_failed");
        return false;
    }

    listing_dir_open_ = true;
    return true;
}

bool StorageBrowserJob::reserve_listing_entries_locked(size_t needed) {
    if (!listing_build_ || needed <= listing_build_->entry_capacity_) return true;

    size_t capacity = listing_build_->entry_capacity_
        ? listing_build_->entry_capacity_ * 2
        : LISTING_INITIAL_ENTRY_CAPACITY;
    while (capacity < needed) capacity *= 2;

    StorageDirectorySnapshot::Entry *entries =
        static_cast<StorageDirectorySnapshot::Entry *>(Memory::calloc_large(
            capacity, sizeof(StorageDirectorySnapshot::Entry), false));
    if (!entries) return false;

    if (listing_build_->entries_) {
        memcpy(entries,
               listing_build_->entries_,
               listing_build_->entry_count_ * sizeof(*entries));
        Memory::free(listing_build_->entries_);
    }
    listing_build_->entries_ = entries;
    listing_build_->entry_capacity_ = capacity;
    return true;
}

bool StorageBrowserJob::reserve_listing_names_locked(size_t needed) {
    if (!listing_build_ || needed <= listing_build_->names_capacity_) return true;

    size_t capacity = listing_build_->names_capacity_
        ? listing_build_->names_capacity_ * 2
        : LISTING_INITIAL_NAME_CAPACITY;
    while (capacity < needed) capacity *= 2;

    char *names = static_cast<char *>(Memory::alloc_large(capacity, false));
    if (!names) return false;

    if (listing_build_->names_) {
        memcpy(names, listing_build_->names_, listing_build_->names_length_);
        Memory::free(listing_build_->names_);
    }
    listing_build_->names_ = names;
    listing_build_->names_capacity_ = capacity;
    return true;
}

bool StorageBrowserJob::append_listing_entry_locked(const char *name,
                                                    bool directory,
                                                    uint64_t size,
                                                    uint64_t modified) {
    if (!listing_build_ || !name || !*name) return false;

    const size_t name_length = strlen(name) + 1;
    if (!reserve_listing_entries_locked(listing_build_->entry_count_ + 1) ||
        !reserve_listing_names_locked(listing_build_->names_length_ +
                                      name_length)) {
        return false;
    }

    StorageDirectorySnapshot::Entry &entry =
        listing_build_->entries_[listing_build_->entry_count_++];
    entry.name_offset = static_cast<uint32_t>(listing_build_->names_length_);
    entry.directory = directory;
    entry.size = directory ? 0 : size;
    entry.modified = modified;

    memcpy(listing_build_->names_ + listing_build_->names_length_,
           name,
           name_length);
    listing_build_->names_length_ += name_length;
    return true;
}

void StorageBrowserJob::fail_listing_locked(const char *error) {
    if (listing_build_) {
        copy_cstr(listing_error_path_, sizeof(listing_error_path_),
                  listing_build_->path());
    }
    copy_cstr(listing_error_, sizeof(listing_error_), error);
    clear_listing_build_locked();
}

void StorageBrowserJob::publish_listing_locked() {
    if (!listing_build_) return;

    listing_build_->revision_ = next_snapshot_revision_++;
    if (listing_build_->revision_ == 0) {
        listing_build_->revision_ = next_snapshot_revision_++;
    }

    int slot = find_snapshot_locked(listing_build_->path());
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

    snapshots_[slot] = listing_build_;
    touch_snapshot_locked(static_cast<size_t>(slot));
    listing_error_path_[0] = 0;
    listing_error_[0] = 0;
    clear_listing_build_locked();
}

JobStep StorageBrowserJob::sort_listing_step_locked() {
    if (!listing_build_) return JobStep::Idle;

    const size_t count = listing_build_->entry_count_;
    if (count < 2) {
        publish_listing_locked();
        return JobStep::Working;
    }

    const uint32_t started_ms = millis();
    while (listing_build_ &&
           static_cast<uint32_t>(millis() - started_ms) <
               LISTING_STEP_BUDGET_MS) {
        if (!sort_merge_active_ && sort_left_ >= count) {
            sort_source_is_entries_ = !sort_source_is_entries_;
            sort_width_ *= 2;
            sort_left_ = 0;

            if (sort_width_ >= count) {
                if (!sort_source_is_entries_) {
                    std::swap(listing_build_->entries_,
                              listing_build_->sort_entries_);
                    listing_build_->entry_capacity_ = count;
                }
                Memory::free(listing_build_->sort_entries_);
                listing_build_->sort_entries_ = nullptr;
                publish_listing_locked();
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
            ? listing_build_->entries_
            : listing_build_->sort_entries_;
        StorageDirectorySnapshot::Entry *destination = sort_source_is_entries_
            ? listing_build_->sort_entries_
            : listing_build_->entries_;

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
                    listing_build_->names_ + left.name_offset,
                    right.modified,
                    listing_build_->names_ + right.name_offset) <= 0;
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

JobStep StorageBrowserJob::listing_step_locked() {
    if (pending_listing_ &&
        (!listing_build_ ||
         strcmp(listing_build_->path(), pending_listing_path_) != 0)) {
        (void)start_pending_listing_locked();
    } else if (!listing_build_) {
        (void)start_pending_listing_locked();
    }
    if (!listing_build_) return JobStep::Idle;

    if (listing_phase_ == ListingPhase::Sorting) {
        return sort_listing_step_locked();
    }
    if (!Storage::mounted()) {
        fail_listing_locked("storage_unavailable");
        return JobStep::Idle;
    }
    if (!ensure_listing_dir_open_locked()) return JobStep::Idle;

    const uint32_t started_ms = millis();
    while (listing_build_ &&
           static_cast<uint32_t>(millis() - started_ms) <
               LISTING_STEP_BUDGET_MS) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(listing_dir_, child)) {
            close_listing_dir_locked();
            if (listing_build_->entry_count_ < 2) {
                publish_listing_locked();
                return JobStep::Working;
            }

            listing_build_->sort_entries_ =
                static_cast<StorageDirectorySnapshot::Entry *>(
                    Memory::calloc_large(
                        listing_build_->entry_count_,
                        sizeof(StorageDirectorySnapshot::Entry),
                        false));
            if (!listing_build_->sort_entries_) {
                fail_listing_locked("sort_alloc");
                return JobStep::Idle;
            }

            listing_phase_ = ListingPhase::Sorting;
            sort_width_ = 1;
            sort_left_ = 0;
            sort_source_is_entries_ = true;
            sort_merge_active_ = false;
            return JobStep::Working;
        }
        listing_scanned_++;

        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(listing_build_->path(),
                                       child.name,
                                       child_path,
                                       sizeof(child_path)) ||
            !storage_user_path_valid(child_path)) {
            continue;
        }

        const uint64_t modified = child.last_write > 0
            ? static_cast<uint64_t>(child.last_write)
            : 0;
        if (!append_listing_entry_locked(child.name,
                                         child.is_dir,
                                         child.size,
                                         modified)) {
            fail_listing_locked("snapshot_alloc");
            return JobStep::Idle;
        }
    }
    return JobStep::Working;
}

StorageDownloadPrepareState StorageBrowserJob::prepare_download(
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

    if (active_download_) {
        StoragePreparedDownload &active = *active_download_;
        if (active.error[0] && strcmp(active.path, path) == 0) {
            status_out.state = StorageDownloadPrepareState::Error;
            status_out.id = active.id;
            copy_cstr(status_out.error, sizeof(status_out.error), active.error);
            active_download_.reset();
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

    download->id = next_download_id_++;
    if (download->id == 0) download->id = next_download_id_++;
    copy_cstr(download->path, sizeof(download->path), path);
    copy_cstr(download->filename, sizeof(download->filename),
              storage_basename_from_path(path));
    active_download_ = download;
    status_out.id = download->id;
    unlock();

    wake_background_worker();
    return status_out.state;
}

bool StorageBrowserJob::begin_download(
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
    if (!active_download_ || active_download_->id != id ||
        active_download_->error[0] ||
        !active_download_->metadata_ready ||
        (active_download_->ring.readable() < DOWNLOAD_READY_BYTES &&
         !active_download_->producer_done) ||
        active_download_->consumer_attached) {
        copy_cstr(error_out, error_out_size, "not_ready");
        unlock();
        return false;
    }

    active_download_->consumer_attached = true;
    active_download_->consumer_activity_ms = nonzero_millis(millis());
    copy_cstr(filename_out, filename_out_size, active_download_->filename);
    size_out = active_download_->size;
    download_out = active_download_;
    unlock();

    wake_background_worker();
    return true;
}

PreparedByteRead StorageBrowserJob::read_download(
    StoragePreparedDownload &download,
    uint8_t *buffer,
    size_t max_length,
    size_t offset) {
    PreparedByteRead result;
    if (!buffer || max_length == 0 || !lock(0)) {
        result.state = PreparedByteReadState::Retry;
        return result;
    }

    if (!active_download_ || active_download_.get() != &download ||
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

void StorageBrowserJob::finish_download(StoragePreparedDownload &download) {
    if (!lock(0)) {
        download.cancel_requested.store(true, std::memory_order_relaxed);
        wake_background_worker();
        return;
    }
    if (active_download_.get() == &download) {
        download.consumer_closed = true;
        if (download.consumed < download.size) {
            download.cancel_requested.store(true, std::memory_order_relaxed);
        }
    }
    unlock();
    wake_background_worker();
}

void StorageBrowserJob::close_download_file(StoragePreparedDownload &download) {
    if (!download.input_open) return;

    Storage::Guard guard;
    download.input.close();
    download.input_open = false;
}

void StorageBrowserJob::fail_download_locked(StoragePreparedDownload &download,
                                             const char *error) {
    copy_cstr(download.error, sizeof(download.error), error);
    if (download.ready_ms == 0) download.ready_ms = nonzero_millis(millis());
    download.producer_done = true;
}

void StorageBrowserJob::retire_download_locked(StoragePreparedDownload &download,
                                               bool success) {
    if (!success && !download.error[0]) {
        copy_cstr(download.error, sizeof(download.error), "download_aborted");
    }
    active_download_.reset();
}

JobStep StorageBrowserJob::download_step() {
    if (!lock(20)) return JobStep::Waiting;
    std::shared_ptr<StoragePreparedDownload> download = active_download_;
    if (!download) {
        unlock();
        return JobStep::Idle;
    }

    if (download->cancel_requested.load(std::memory_order_relaxed)) {
        unlock();
        close_download_file(*download);
        if (!lock(20)) return JobStep::Waiting;
        if (active_download_.get() == download.get()) {
            retire_download_locked(*download, false);
        }
        unlock();
        return JobStep::Idle;
    }

    if (download->producer_done) {
        const bool attach_expired = !download->consumer_attached &&
            millis_deadline_reached(nonzero_millis(millis()),
                             download->ready_ms +
                                 DOWNLOAD_ATTACH_TIMEOUT_MS);
        if (attach_expired) {
            retire_download_locked(*download, !download->error[0]);
            unlock();
            return JobStep::Idle;
        }

        const bool complete = download->consumed == download->size;
        const bool retire = download->consumer_closed &&
            (complete || download->error[0]);
        if (retire) retire_download_locked(*download, complete);
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
            fail_download_locked(*download, "buffer_alloc");
        } else if (!input || directory) {
            Memory::free(ring_storage);
            if (input) {
                Storage::Guard guard;
                input.close();
            }
            fail_download_locked(*download,
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

        if (download->producer_done) close_download_file(*download);
        return failed ? JobStep::Idle : JobStep::Working;
    }

    if (!download->consumer_attached &&
        millis_deadline_reached(nonzero_millis(millis()),
                         download->ready_ms + DOWNLOAD_ATTACH_TIMEOUT_MS)) {
        download->cancel_requested.store(true, std::memory_order_relaxed);
        unlock();
        return JobStep::Working;
    }
    if (download->consumer_attached && !download->consumer_closed &&
        millis_deadline_reached(nonzero_millis(millis()),
                         download->consumer_activity_ms +
                             DOWNLOAD_ATTACH_TIMEOUT_MS)) {
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
        fail_download_locked(*download, "read_failed");
        unlock();
        close_download_file(*download);
        return JobStep::Idle;
    }
    if (!download->ring.commit_write(bytes_read)) {
        fail_download_locked(*download, "ring_commit");
        unlock();
        close_download_file(*download);
        return JobStep::Idle;
    }

    download->produced += bytes_read;
    if (download->produced >= download->size) {
        download->producer_done = true;
    }
    const bool done = download->producer_done;
    unlock();

    if (done) close_download_file(*download);
    return JobStep::Working;
}

JobStep StorageBrowserJob::step() {
    begin();

    if (lock(0)) {
        const bool downloading = static_cast<bool>(active_download_);
        unlock();
        if (downloading) return download_step();
    }

    if (!lock(20)) return JobStep::Waiting;
    const JobStep result = listing_step_locked();
    unlock();
    return result;
}

void StorageBrowserJob::on_preempt() {
    begin();
    if (!lock(20)) return;

    close_listing_dir_locked();
    std::shared_ptr<StoragePreparedDownload> download = active_download_;
    if (download) {
        download->cancel_requested.store(true, std::memory_order_relaxed);
    }
    unlock();

    if (!download) return;
    close_download_file(*download);
    if (!lock(20)) return;
    if (active_download_.get() == download.get()) {
        fail_download_locked(*download, "preempted");
        if (!download->consumer_attached || download->consumer_closed) {
            active_download_.reset();
        }
    }
    unlock();
}

}  // namespace aircannect
