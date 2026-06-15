#include "storage_archive_job.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t ARCHIVE_IO_BYTES = 8192;
static constexpr size_t ARCHIVE_PREPARE_ENTRY_BUDGET = 12;
static constexpr size_t ARCHIVE_CENTRAL_ENTRY_BUDGET = 16;
static constexpr size_t ARCHIVE_INITIAL_ENTRY_CAPACITY = 64;
static constexpr size_t ARCHIVE_INITIAL_PATH_BYTES = 4096;
static constexpr size_t ARCHIVE_MAX_DEPTH = 16;
static constexpr uint64_t ARCHIVE_FREE_SPACE_MARGIN_BYTES = 64ULL * 1024ULL;
static constexpr uint32_t ARCHIVE_DOWNLOAD_CLEANUP_DELAY_MS = 5000;
static constexpr uint32_t ARCHIVE_STALE_CLEANUP_MS = 60UL * 60UL * 1000UL;
static constexpr uint32_t ZIP32_MAX = 0xFFFFFFFFu;
static constexpr uint32_t ZIP_LOCAL_HEADER_SIZE = 30;
static constexpr uint32_t ZIP_DATA_DESCRIPTOR_SIZE = 16;
static constexpr uint32_t ZIP_CENTRAL_HEADER_SIZE = 46;
static constexpr uint32_t ZIP_EOCD_SIZE = 22;
static constexpr uint16_t ZIP_GENERAL_DATA_DESCRIPTOR = 0x0008;
static constexpr uint16_t ZIP_METHOD_STORE = 0;

const char *basename_from_path(const char *path) {
    if (!path || !*path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool path_equals_or_under(const char *path, const char *root) {
    if (!path || !root) return false;
    const size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) return false;
    return path[root_len] == '\0' || path[root_len] == '/';
}

bool append_child_path(const char *parent,
                       const char *name,
                       char *out,
                       size_t out_size) {
    if (!parent || !name || !*name || !out || out_size == 0) return false;
    const int written = strcmp(parent, "/") == 0
        ? snprintf(out, out_size, "/%s", name)
        : snprintf(out, out_size, "%s/%s", parent, name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

const char *relative_archive_name(const char *source, const char *path) {
    if (!source || !path) return "";
    if (strcmp(source, "/") == 0) {
        return path[0] == '/' ? path + 1 : path;
    }
    const size_t source_len = strlen(source);
    if (strncmp(source, path, source_len) != 0) return "";
    if (path[source_len] == '/') return path + source_len + 1;
    return basename_from_path(path);
}

bool archive_temp_name(const char *path) {
    const char *base = basename_from_path(path);
    return strncmp(base, "archive-", 8) == 0 && strstr(base, ".zip") != nullptr;
}

void normalize_source_path(char *path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

void put_le16(uint8_t *dst, size_t offset, uint16_t value) {
    dst[offset + 0] = static_cast<uint8_t>(value & 0xffu);
    dst[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void put_le32(uint8_t *dst, size_t offset, uint32_t value) {
    dst[offset + 0] = static_cast<uint8_t>(value & 0xffu);
    dst[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
    dst[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
    dst[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

bool write_exact(File &file, const uint8_t *data, size_t len) {
    Storage::Guard guard;
    return file && file.write(data, len) == len;
}

bool write_exact(File &file, const char *data, size_t len) {
    return write_exact(file, reinterpret_cast<const uint8_t *>(data), len);
}

uint32_t millis_nonzero() {
    uint32_t now = millis();
    return now == 0 ? 1 : now;
}

}  // namespace

struct StorageArchiveJob::ArchiveEntry {
    uint32_t path_offset = 0;
    uint32_t size = 0;
    uint32_t crc = 0;
    uint32_t local_header_offset = 0;
    uint16_t path_len = 0;
    bool directory = false;
};

struct StorageArchiveJob::WalkFrame {
    char path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
    uint32_t next_index = 0;
    bool opened = false;
    File dir;
};

const char *storage_archive_state_name(StorageArchiveState state) {
    switch (state) {
        case StorageArchiveState::Idle: return "idle";
        case StorageArchiveState::Preparing: return "preparing";
        case StorageArchiveState::Building: return "building";
        case StorageArchiveState::Ready: return "ready";
        case StorageArchiveState::Downloading: return "downloading";
        case StorageArchiveState::Error: return "error";
    }
    return "unknown";
}

bool storage_archive_valid_path(const char *path) {
    if (!path || path[0] != '/') return false;
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_ARCHIVE_PATH_MAX) return false;
    if (path_equals_or_under(path, AC_STORAGE_ARCHIVE_TEMP_DIR)) return false;

    size_t segment_start = 1;
    for (size_t i = 0; i <= len; ++i) {
        const char c = path[i];
        if (c == '/' || c == '\0') {
            const size_t segment_len = i - segment_start;
            if (segment_len == 0 && i != 0 && c != '\0') return false;
            if ((segment_len == 1 && path[segment_start] == '.') ||
                (segment_len == 2 && path[segment_start] == '.' &&
                 path[segment_start + 1] == '.')) {
                return false;
            }
            segment_start = i + 1;
        } else if (static_cast<unsigned char>(c) < 0x20 || c == '\\') {
            return false;
        }
    }
    return true;
}

void StorageArchiveJob::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    if (lock() && !walk_stack_) {
        walk_capacity_ = ARCHIVE_MAX_DEPTH;
        walk_stack_ = new (std::nothrow) WalkFrame[walk_capacity_];
        if (!walk_stack_) {
            set_error_locked("metadata_alloc");
        }
        unlock();
    }
}

bool StorageArchiveJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageArchiveJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageArchiveJob::set_error_locked(const char *error) {
    close_active_files_locked();
    close_walk_locked();
    status_.state = StorageArchiveState::Error;
    copy_cstr(status_.error, sizeof(status_.error), error);
    status_.updated_ms = millis_nonzero();
    Log::logf(CAT_GENERAL, LOG_WARN, "[ARCHIVE] error=%s\n",
              status_.error[0] ? status_.error : "error");
}

void StorageArchiveJob::reset_job_locked(bool keep_status) {
    close_active_files_locked();
    close_walk_locked();
    if (entries_) {
        Memory::free(entries_);
        entries_ = nullptr;
    }
    if (path_bytes_) {
        Memory::free(path_bytes_);
        path_bytes_ = nullptr;
    }
    if (io_buffer_) {
        Memory::free(io_buffer_);
        io_buffer_ = nullptr;
    }
    entry_count_ = 0;
    entry_capacity_ = 0;
    path_bytes_len_ = 0;
    path_bytes_capacity_ = 0;
    current_file_active_ = false;
    current_file_index_ = 0;
    current_file_offset_ = 0;
    current_crc_ = 0;
    central_index_ = 0;
    central_start_offset_ = 0;
    central_size_ = 0;
    central_started_ = false;
    cleanup_due_ms_ = 0;

    if (!keep_status) {
        status_ = StorageArchiveStatus();
        status_.state = StorageArchiveState::Idle;
        status_.updated_ms = millis_nonzero();
    }
}

void StorageArchiveJob::close_active_files_locked() {
    if (input_open_) {
        input_.close();
        input_open_ = false;
    }
    if (output_open_) {
        output_.close();
        output_open_ = false;
    }
}

void StorageArchiveJob::close_walk_locked() {
    if (walk_stack_) {
        for (size_t i = 0; i < walk_depth_; ++i) {
            if (walk_stack_[i].opened) {
                walk_stack_[i].dir.close();
                walk_stack_[i].opened = false;
            }
        }
    }
}

bool StorageArchiveJob::cleanup_ready_archive_locked() {
    if (!status_.archive_path[0]) return true;
    char remove_path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
    copy_cstr(remove_path, sizeof(remove_path), status_.archive_path);
    {
        Storage::Guard guard;
        if (!Storage::remove(remove_path)) {
            copy_cstr(status_.error, sizeof(status_.error), "cleanup_failed");
            return false;
        }
    }
    status_.archive_path[0] = 0;
    status_.archive_bytes = 0;
    cleanup_due_ms_ = 0;
    return true;
}

void StorageArchiveJob::cleanup_stale_temp_locked() {
    if (!Storage::mounted()) return;
    if (!ensure_temp_dirs_locked()) return;
    Storage::Guard guard;
    File dir = Storage::open(AC_STORAGE_ARCHIVE_TEMP_DIR, "r");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }
    for (;;) {
        File child = dir.openNextFile();
        if (!child) break;
        const bool is_dir = child.isDirectory();
        char path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
        const char *name = basename_from_path(child.name());
        if (!is_dir && append_child_path(AC_STORAGE_ARCHIVE_TEMP_DIR,
                                         name,
                                         path,
                                         sizeof(path)) &&
            archive_temp_name(path)) {
            child.close();
            (void)Storage::remove(path);
        } else {
            child.close();
        }
    }
    dir.close();
}

bool StorageArchiveJob::start(const char *path,
                              bool recursive,
                              uint32_t *id_out,
                              char *error_out,
                              size_t error_out_size) {
    if (id_out) *id_out = 0;
    copy_cstr(error_out, error_out_size, "");
    begin();
    if (!storage_archive_valid_path(path)) {
        copy_cstr(error_out, error_out_size, "bad_path");
        return false;
    }
    if (!lock(50)) {
        copy_cstr(error_out, error_out_size, "busy");
        return false;
    }
    if (status_.state == StorageArchiveState::Preparing ||
        status_.state == StorageArchiveState::Building ||
        status_.state == StorageArchiveState::Downloading) {
        copy_cstr(error_out, error_out_size, "archive_busy");
        unlock();
        return false;
    }
    if ((status_.state == StorageArchiveState::Ready ||
         status_.state == StorageArchiveState::Error) &&
        status_.archive_path[0]) {
        (void)cleanup_ready_archive_locked();
    }
    reset_job_locked(false);

    status_.state = StorageArchiveState::Preparing;
    status_.id = next_id_++;
    if (status_.id == 0) status_.id = next_id_++;
    status_.recursive = recursive;
    status_.psram_metadata = Memory::psram_available();
    copy_cstr(status_.source_path, sizeof(status_.source_path), path);
    normalize_source_path(status_.source_path);
    const char *base = strcmp(status_.source_path, "/") == 0
        ? "storage"
        : basename_from_path(status_.source_path);
    if (!base || !*base) base = "storage";
    snprintf(status_.filename, sizeof(status_.filename), "%s.zip", base);
    snprintf(status_.archive_path,
             sizeof(status_.archive_path),
             "%s/archive-%08lx.zip",
             AC_STORAGE_ARCHIVE_TEMP_DIR,
             static_cast<unsigned long>(status_.id));
    status_.started_ms = millis_nonzero();
    status_.updated_ms = status_.started_ms;

    if (!walk_stack_) {
        set_error_locked("metadata_alloc");
        copy_cstr(error_out, error_out_size, status_.error);
        unlock();
        return false;
    }
    if (!push_walk_dir_locked(status_.source_path)) {
        copy_cstr(error_out, error_out_size, status_.error);
        unlock();
        return false;
    }
    if (id_out) *id_out = status_.id;
    unlock();
    return true;
}

StorageArchiveStatus StorageArchiveJob::status() const {
    StorageArchiveStatus out;
    if (lock(20)) {
        out = status_;
        unlock();
    }
    return out;
}

bool StorageArchiveJob::download_info(uint32_t id,
                                      char *path_out,
                                      size_t path_out_size,
                                      char *filename_out,
                                      size_t filename_out_size,
                                      uint64_t &size_out) const {
    size_out = 0;
    if (!lock(20)) return false;
    const bool ok = status_.id == id &&
                    status_.state == StorageArchiveState::Ready &&
                    status_.archive_path[0] &&
                    status_.archive_bytes > 0;
    if (ok) {
        copy_cstr(path_out, path_out_size, status_.archive_path);
        copy_cstr(filename_out, filename_out_size, status_.filename);
        size_out = status_.archive_bytes;
    }
    unlock();
    return ok;
}

bool StorageArchiveJob::mark_download_started(uint32_t id,
                                              char *error_out,
                                              size_t error_out_size) {
    copy_cstr(error_out, error_out_size, "");
    if (!lock(20)) {
        copy_cstr(error_out, error_out_size, "busy");
        return false;
    }
    if (status_.id != id || status_.state != StorageArchiveState::Ready) {
        copy_cstr(error_out, error_out_size, "not_ready");
        unlock();
        return false;
    }
    status_.state = StorageArchiveState::Downloading;
    status_.updated_ms = millis_nonzero();
    cleanup_due_ms_ = 0;
    unlock();
    return true;
}

void StorageArchiveJob::mark_download_finished(uint32_t id) {
    if (!lock(20)) return;
    if (status_.id == id && status_.state == StorageArchiveState::Downloading) {
        status_.state = StorageArchiveState::Ready;
        status_.updated_ms = millis_nonzero();
        cleanup_due_ms_ = status_.updated_ms + ARCHIVE_DOWNLOAD_CLEANUP_DELAY_MS;
        if (cleanup_due_ms_ == 0) cleanup_due_ms_ = 1;
    }
    unlock();
}

JobStep StorageArchiveJob::step() {
    begin();
    if (!lock(50)) return JobStep::Waiting;

    const uint32_t now = millis_nonzero();
    if (status_.state == StorageArchiveState::Ready && cleanup_due_ms_ != 0 &&
        static_cast<int32_t>(now - cleanup_due_ms_) >= 0) {
        if (cleanup_ready_archive_locked()) {
            reset_job_locked(false);
        }
        unlock();
        return JobStep::Working;
    }
    if (status_.state == StorageArchiveState::Idle &&
        stale_cleanup_due_ms_ != 0 &&
        static_cast<int32_t>(now - stale_cleanup_due_ms_) >= 0) {
        cleanup_stale_temp_locked();
        stale_cleanup_due_ms_ = now + ARCHIVE_STALE_CLEANUP_MS;
        if (stale_cleanup_due_ms_ == 0) stale_cleanup_due_ms_ = 1;
        unlock();
        return JobStep::Idle;
    }

    JobStep result = JobStep::Idle;
    if (status_.state == StorageArchiveState::Preparing) {
        result = prepare_step_locked() ? JobStep::Working : JobStep::Idle;
    } else if (status_.state == StorageArchiveState::Building) {
        result = build_step_locked() ? JobStep::Working : JobStep::Idle;
    }
    unlock();
    return result;
}

void StorageArchiveJob::on_preempt() {
    if (!lock(20)) return;
    if (status_.state == StorageArchiveState::Preparing ||
        status_.state == StorageArchiveState::Building) {
        close_active_files_locked();
        close_walk_locked();
        status_.updated_ms = millis_nonzero();
    }
    unlock();
}

bool StorageArchiveJob::reserve_entries_locked(size_t needed) {
    if (needed <= entry_capacity_) return true;
    size_t next = entry_capacity_ ? entry_capacity_ * 2
                                  : ARCHIVE_INITIAL_ENTRY_CAPACITY;
    while (next < needed) next *= 2;
    ArchiveEntry *entries = static_cast<ArchiveEntry *>(
        Memory::calloc_large(next, sizeof(ArchiveEntry), false));
    if (!entries) return false;
    if (entries_) {
        memcpy(entries, entries_, entry_count_ * sizeof(ArchiveEntry));
        Memory::free(entries_);
    }
    entries_ = entries;
    entry_capacity_ = next;
    status_.psram_metadata = true;
    return true;
}

bool StorageArchiveJob::reserve_path_bytes_locked(size_t needed) {
    if (needed <= path_bytes_capacity_) return true;
    size_t next = path_bytes_capacity_ ? path_bytes_capacity_ * 2
                                       : ARCHIVE_INITIAL_PATH_BYTES;
    while (next < needed) next *= 2;
    char *bytes = static_cast<char *>(Memory::alloc_large(next, false));
    if (!bytes) return false;
    if (path_bytes_) {
        memcpy(bytes, path_bytes_, path_bytes_len_);
        Memory::free(path_bytes_);
    }
    path_bytes_ = bytes;
    path_bytes_capacity_ = next;
    status_.psram_metadata = true;
    return true;
}

bool StorageArchiveJob::append_entry_locked(const char *path,
                                            uint64_t size,
                                            bool directory) {
    if (size > ZIP32_MAX || entry_count_ >= 0xFFFFu) {
        set_error_locked("zip32_limit");
        return false;
    }
    const char *relative = relative_archive_name(status_.source_path, path);
    const size_t relative_len = strlen(relative);
    const bool needs_dir_slash =
        directory && relative_len > 0 && relative[relative_len - 1] != '/';
    const size_t path_len = relative_len + (needs_dir_slash ? 1 : 0);
    if (path_len == 0 || path_len > 0xFFFFu) {
        set_error_locked("bad_entry_path");
        return false;
    }
    if (!reserve_entries_locked(entry_count_ + 1) ||
        !reserve_path_bytes_locked(path_bytes_len_ + path_len + 1)) {
        set_error_locked("metadata_alloc");
        return false;
    }

    ArchiveEntry &entry = entries_[entry_count_++];
    entry.path_offset = static_cast<uint32_t>(path_bytes_len_);
    entry.path_len = static_cast<uint16_t>(path_len);
    entry.size = directory ? 0 : static_cast<uint32_t>(size);
    entry.directory = directory;
    memcpy(path_bytes_ + path_bytes_len_, relative, relative_len);
    if (needs_dir_slash) path_bytes_[path_bytes_len_ + relative_len] = '/';
    path_bytes_[path_bytes_len_ + path_len] = '\0';
    path_bytes_len_ += path_len + 1;

    if (!directory) {
        status_.files++;
        status_.input_bytes += size;
    }
    status_.estimated_archive_bytes += (directory ? 0 : size) +
        ZIP_LOCAL_HEADER_SIZE +
        (directory ? 0 : ZIP_DATA_DESCRIPTOR_SIZE) +
        ZIP_CENTRAL_HEADER_SIZE + path_len * 2;
    return true;
}

bool StorageArchiveJob::push_walk_dir_locked(const char *path) {
    if (!walk_stack_ || walk_depth_ >= walk_capacity_) {
        set_error_locked("max_depth");
        return false;
    }
    WalkFrame &frame = walk_stack_[walk_depth_++];
    frame = WalkFrame();
    copy_cstr(frame.path, sizeof(frame.path), path);
    status_.dirs++;
    return true;
}

bool StorageArchiveJob::ensure_walk_dir_open_locked(WalkFrame &frame) {
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
    for (uint32_t i = 0; i < frame.next_index; ++i) {
        File skipped = frame.dir.openNextFile();
        if (!skipped) {
            frame.dir.close();
            set_error_locked("walk_resume_failed");
            return false;
        }
        skipped.close();
    }
    frame.opened = true;
    return true;
}

bool StorageArchiveJob::prepare_step_locked() {
    if (!Storage::mounted()) {
        set_error_locked("storage_unavailable");
        return false;
    }

    size_t budget = ARCHIVE_PREPARE_ENTRY_BUDGET;
    while (budget > 0 && walk_depth_ > 0 &&
           status_.state == StorageArchiveState::Preparing) {
        WalkFrame &frame = walk_stack_[walk_depth_ - 1];
        if (!ensure_walk_dir_open_locked(frame)) return false;

        File child;
        {
            Storage::Guard guard;
            child = frame.dir.openNextFile();
        }
        if (!child) {
            if (frame.opened) {
                frame.dir.close();
                frame.opened = false;
            }
            walk_depth_--;
            continue;
        }
        frame.next_index++;
        budget--;

        const char *name = basename_from_path(child.name());
        char child_path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
        const bool path_ok = append_child_path(frame.path,
                                               name,
                                               child_path,
                                               sizeof(child_path));
        const bool is_dir = child.isDirectory();
        const uint64_t size = is_dir ? 0 : static_cast<uint64_t>(child.size());
        child.close();

        if (!path_ok) {
            set_error_locked("bad_child_path");
            return false;
        }
        if (path_equals_or_under(child_path, AC_STORAGE_ARCHIVE_TEMP_DIR)) {
            continue;
        }
        if (!storage_archive_valid_path(child_path)) {
            set_error_locked("bad_child_path");
            return false;
        }
        if (is_dir) {
            if (status_.recursive) {
                if (!append_entry_locked(child_path, 0, true) ||
                    !push_walk_dir_locked(child_path)) {
                    return false;
                }
            }
            continue;
        }
        if (!append_entry_locked(child_path, size, false)) return false;
    }

    if (walk_depth_ == 0 &&
        status_.state == StorageArchiveState::Preparing) {
        return finalize_prepare_locked();
    }
    status_.updated_ms = millis_nonzero();
    return true;
}

bool StorageArchiveJob::finalize_prepare_locked() {
    status_.estimated_archive_bytes += ZIP_EOCD_SIZE;
    if (status_.estimated_archive_bytes > ZIP32_MAX) {
        set_error_locked("zip32_limit");
        return false;
    }
    const StorageStatus storage = Storage::status();
    status_.free_bytes_at_start = storage.free_bytes;
    const uint64_t needed =
        status_.estimated_archive_bytes + ARCHIVE_FREE_SPACE_MARGIN_BYTES;
    if (storage.total_bytes != 0 && storage.free_bytes < needed) {
        set_error_locked("insufficient_space");
        return false;
    }
    if (!io_buffer_) {
        io_buffer_ = static_cast<uint8_t *>(
            Memory::alloc_large(ARCHIVE_IO_BYTES, true));
        if (!io_buffer_) {
            set_error_locked("buffer_alloc");
            return false;
        }
    }
    if (!ensure_temp_dirs_locked()) {
        set_error_locked("temp_dir");
        return false;
    }
    {
        Storage::Guard guard;
        (void)Storage::remove(status_.archive_path);
    }
    status_.state = StorageArchiveState::Building;
    status_.updated_ms = millis_nonzero();
    Log::logf(CAT_GENERAL,
              LOG_INFO,
              "[ARCHIVE] build id=%lu path=%s recursive=%u files=%lu "
              "input=%llu estimate=%llu\n",
              static_cast<unsigned long>(status_.id),
              status_.source_path,
              status_.recursive ? 1u : 0u,
              static_cast<unsigned long>(status_.files),
              static_cast<unsigned long long>(status_.input_bytes),
              static_cast<unsigned long long>(status_.estimated_archive_bytes));
    return true;
}

bool StorageArchiveJob::ensure_temp_dirs_locked() {
    Storage::Guard guard;
    return Storage::ensure_dir("/aircannect") &&
           Storage::ensure_dir("/aircannect/tmp") &&
           Storage::ensure_dir(AC_STORAGE_ARCHIVE_TEMP_DIR);
}

bool StorageArchiveJob::open_output_locked() {
    if (output_open_) return true;
    Storage::Guard guard;
    output_ = Storage::open(status_.archive_path,
                            status_.archive_bytes == 0 ? "w" : "a");
    if (!output_) {
        set_error_locked("archive_open");
        return false;
    }
    output_open_ = true;
    return true;
}

bool StorageArchiveJob::build_step_locked() {
    if (!Storage::mounted()) {
        set_error_locked("storage_unavailable");
        return false;
    }
    if (!open_output_locked()) return false;

    if (current_file_index_ < entry_count_) {
        if (!current_file_active_ && !begin_current_file_locked()) return false;
        return copy_current_file_step_locked();
    }
    if (!write_central_step_locked()) return false;
    if (central_index_ >= entry_count_) {
        if (!write_eocd_locked()) return false;
        {
            Storage::Guard guard;
            output_.flush();
            output_.close();
            output_open_ = false;
        }
        status_.state = StorageArchiveState::Ready;
        status_.archive_bytes = status_.bytes_done;
        status_.updated_ms = millis_nonzero();
        Log::logf(CAT_GENERAL,
                  LOG_INFO,
                  "[ARCHIVE] ready id=%lu files=%lu bytes=%llu path=%s\n",
                  static_cast<unsigned long>(status_.id),
                  static_cast<unsigned long>(status_.files),
                  static_cast<unsigned long long>(status_.archive_bytes),
                  status_.archive_path);
    }
    return true;
}

bool StorageArchiveJob::begin_current_file_locked() {
    ArchiveEntry &entry = entries_[current_file_index_];
    const char *name = path_bytes_ + entry.path_offset;
    entry.local_header_offset = static_cast<uint32_t>(status_.bytes_done);
    entry.crc = 0;

    uint8_t header[ZIP_LOCAL_HEADER_SIZE] = {};
    put_le32(header, 0, 0x04034b50u);
    put_le16(header, 4, 20);
    put_le16(header, 6, entry.directory ? 0 : ZIP_GENERAL_DATA_DESCRIPTOR);
    put_le16(header, 8, ZIP_METHOD_STORE);
    put_le16(header, 10, 0);
    put_le16(header, 12, 0);
    put_le32(header, 14, entry.directory ? entry.crc : 0);
    put_le32(header, 18, entry.directory ? entry.size : 0);
    put_le32(header, 22, entry.directory ? entry.size : 0);
    put_le16(header, 26, entry.path_len);
    put_le16(header, 28, 0);
    if (!write_exact(output_, header, sizeof(header)) ||
        !write_exact(output_, name, entry.path_len)) {
        set_error_locked("archive_write");
        return false;
    }
    status_.bytes_done += sizeof(header) + entry.path_len;
    if (entry.directory) {
        current_file_active_ = true;
        return true;
    }

    char absolute[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
    if (strcmp(status_.source_path, "/") == 0) {
        snprintf(absolute, sizeof(absolute), "/%s", name);
    } else {
        snprintf(absolute, sizeof(absolute), "%s/%s",
                 status_.source_path, name);
    }
    {
        Storage::Guard guard;
        input_ = Storage::open(absolute, "r");
        if (!input_ || input_.isDirectory()) {
            if (input_) input_.close();
            set_error_locked("input_open");
            return false;
        }
    }
    input_open_ = true;
    current_file_offset_ = 0;
    current_crc_ = crc32_ieee_initial_state();
    current_file_active_ = true;
    return true;
}

bool StorageArchiveJob::copy_current_file_step_locked() {
    ArchiveEntry &entry = entries_[current_file_index_];
    if (entry.directory) return finish_current_file_locked();

    if (current_file_offset_ < entry.size) {
        if (!input_open_) {
            const char *name = path_bytes_ + entry.path_offset;
            char absolute[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
            if (strcmp(status_.source_path, "/") == 0) {
                snprintf(absolute, sizeof(absolute), "/%s", name);
            } else {
                snprintf(absolute, sizeof(absolute), "%s/%s",
                         status_.source_path, name);
            }
            Storage::Guard guard;
            input_ = Storage::open(absolute, "r");
            if (!input_ || input_.isDirectory() ||
                !input_.seek(current_file_offset_)) {
                if (input_) input_.close();
                set_error_locked("input_resume");
                return false;
            }
            input_open_ = true;
        }

        const uint64_t remaining = entry.size - current_file_offset_;
        const size_t want = static_cast<size_t>(
            remaining < ARCHIVE_IO_BYTES ? remaining : ARCHIVE_IO_BYTES);
        size_t read = 0;
        {
            Storage::Guard guard;
            read = input_.read(io_buffer_, want);
        }
        if (read == 0) {
            set_error_locked("input_read");
            return false;
        }
        if (!write_exact(output_, io_buffer_, read)) {
            set_error_locked("archive_write");
            return false;
        }
        current_crc_ = crc32_ieee_update_state(current_crc_, io_buffer_, read);
        current_file_offset_ += read;
        status_.bytes_done += read;
        status_.archive_bytes = status_.bytes_done;
        status_.updated_ms = millis_nonzero();
        return true;
    }
    return finish_current_file_locked();
}

bool StorageArchiveJob::finish_current_file_locked() {
    ArchiveEntry &entry = entries_[current_file_index_];
    if (input_open_) {
        input_.close();
        input_open_ = false;
    }
    if (!entry.directory) {
        entry.crc = crc32_ieee_finish_state(current_crc_);

        uint8_t desc[ZIP_DATA_DESCRIPTOR_SIZE] = {};
        put_le32(desc, 0, 0x08074b50u);
        put_le32(desc, 4, entry.crc);
        put_le32(desc, 8, entry.size);
        put_le32(desc, 12, entry.size);
        if (!write_exact(output_, desc, sizeof(desc))) {
            set_error_locked("archive_write");
            return false;
        }
        status_.bytes_done += sizeof(desc);
        status_.files_done++;
    }
    status_.archive_bytes = status_.bytes_done;
    current_file_index_++;
    current_file_offset_ = 0;
    current_file_active_ = false;
    status_.updated_ms = millis_nonzero();
    return true;
}

bool StorageArchiveJob::write_central_step_locked() {
    if (!central_started_) {
        central_started_ = true;
        central_start_offset_ = status_.bytes_done;
        central_size_ = 0;
    }

    size_t budget = ARCHIVE_CENTRAL_ENTRY_BUDGET;
    while (budget-- > 0 && central_index_ < entry_count_) {
        const ArchiveEntry &entry = entries_[central_index_++];
        const char *name = path_bytes_ + entry.path_offset;
        uint8_t header[ZIP_CENTRAL_HEADER_SIZE] = {};
        put_le32(header, 0, 0x02014b50u);
        put_le16(header, 4, 20);
        put_le16(header, 6, 20);
        put_le16(header, 8, entry.directory ? 0 : ZIP_GENERAL_DATA_DESCRIPTOR);
        put_le16(header, 10, ZIP_METHOD_STORE);
        put_le16(header, 12, 0);
        put_le16(header, 14, 0);
        put_le32(header, 16, entry.crc);
        put_le32(header, 20, entry.size);
        put_le32(header, 24, entry.size);
        put_le16(header, 28, entry.path_len);
        put_le16(header, 30, 0);
        put_le16(header, 32, 0);
        put_le16(header, 34, 0);
        put_le16(header, 36, 0);
        put_le32(header, 38, entry.directory ? 0x10u : 0u);
        put_le32(header, 42, entry.local_header_offset);
        if (!write_exact(output_, header, sizeof(header)) ||
            !write_exact(output_, name, entry.path_len)) {
            set_error_locked("archive_write");
            return false;
        }
        const uint64_t written = sizeof(header) + entry.path_len;
        status_.bytes_done += written;
        central_size_ += written;
    }
    status_.archive_bytes = status_.bytes_done;
    status_.updated_ms = millis_nonzero();
    return true;
}

bool StorageArchiveJob::write_eocd_locked() {
    if (entry_count_ > 0xFFFFu ||
        central_start_offset_ > ZIP32_MAX ||
        central_size_ > ZIP32_MAX) {
        set_error_locked("zip32_limit");
        return false;
    }
    uint8_t eocd[ZIP_EOCD_SIZE] = {};
    put_le32(eocd, 0, 0x06054b50u);
    put_le16(eocd, 4, 0);
    put_le16(eocd, 6, 0);
    put_le16(eocd, 8, static_cast<uint16_t>(entry_count_));
    put_le16(eocd, 10, static_cast<uint16_t>(entry_count_));
    put_le32(eocd, 12, static_cast<uint32_t>(central_size_));
    put_le32(eocd, 16, static_cast<uint32_t>(central_start_offset_));
    put_le16(eocd, 20, 0);
    if (!write_exact(output_, eocd, sizeof(eocd))) {
        set_error_locked("archive_write");
        return false;
    }
    status_.bytes_done += sizeof(eocd);
    return true;
}

}  // namespace aircannect
