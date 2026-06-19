#include "storage_archive_job.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t ARCHIVE_FILE_BUFFER_BYTES = 512;
static constexpr size_t ARCHIVE_READ_AHEAD_BYTES = 256 * 1024;
static constexpr size_t ARCHIVE_PREPARE_ENTRY_BUDGET = 12;
static constexpr size_t ARCHIVE_INITIAL_ENTRY_CAPACITY = 64;
static constexpr size_t ARCHIVE_INITIAL_PATH_BYTES = 4096;
static constexpr size_t ARCHIVE_MAX_DEPTH = 16;
static constexpr uint32_t ARCHIVE_STALE_CLEANUP_MS = 60UL * 60UL * 1000UL;
static constexpr uint32_t ZIP32_MAX = 0xFFFFFFFFu;
static constexpr uint32_t ZIP_LOCAL_HEADER_SIZE = 30;
static constexpr uint32_t ZIP_DATA_DESCRIPTOR_SIZE = 16;
static constexpr uint32_t ZIP_CENTRAL_HEADER_SIZE = 46;
static constexpr uint32_t ZIP_EOCD_SIZE = 22;
static constexpr uint16_t ZIP_GENERAL_DATA_DESCRIPTOR = 0x0008;
static constexpr uint16_t ZIP_METHOD_STORE = 0;
static constexpr uint16_t ZIP_DOS_TIME_DEFAULT = 0;
static constexpr uint16_t ZIP_DOS_DATE_DEFAULT = (1u << 5) | 1u;  // 1980-01-01

struct ZipDosTimestamp {
    uint16_t time = ZIP_DOS_TIME_DEFAULT;
    uint16_t date = ZIP_DOS_DATE_DEFAULT;
};

const char *relative_archive_name(const char *source, const char *path) {
    if (!source || !path) return "";
    if (strcmp(source, "/") == 0) {
        return path[0] == '/' ? path + 1 : path;
    }
    const size_t source_len = strlen(source);
    if (strncmp(source, path, source_len) != 0) return "";
    if (path[source_len] == '/') return path + source_len + 1;
    return storage_basename_from_path(path);
}

bool archive_temp_name(const char *path) {
    const char *base = storage_basename_from_path(path);
    return strncmp(base, "archive-", 8) == 0 && strstr(base, ".zip") != nullptr;
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

ZipDosTimestamp zip_dos_timestamp(time_t seconds) {
    ZipDosTimestamp out;
    if (seconds <= 0) return out;
    struct tm tmv;
    if (!localtime_r(&seconds, &tmv)) return out;
    int year = tmv.tm_year + 1900;
    if (year < 1980) return out;
    if (year > 2107) year = 2107;
    int month = tmv.tm_mon + 1;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    int day = tmv.tm_mday;
    if (day < 1) day = 1;
    if (day > 31) day = 31;
    int hour = tmv.tm_hour;
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    int minute = tmv.tm_min;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;
    int second = tmv.tm_sec;
    if (second < 0) second = 0;
    if (second > 59) second = 59;

    out.time = static_cast<uint16_t>((hour << 11) |
                                     (minute << 5) |
                                     (second / 2));
    out.date = static_cast<uint16_t>(((year - 1980) << 9) |
                                     (month << 5) |
                                     day);
    return out;
}

uint32_t millis_nonzero() {
    uint32_t now = millis();
    return now == 0 ? 1 : now;
}

void log_archive_alloc_failed(const char *context, size_t bytes) {
    Log::logf(CAT_STORAGE,
              LOG_ERROR,
              "[ARCHIVE] allocation failed context=%s bytes=%u\n",
              context ? context : "--",
              static_cast<unsigned>(bytes));
}

void set_archive_file_buffer(File &file) {
    if (file) (void)file.setBufferSize(ARCHIVE_FILE_BUFFER_BYTES);
}

size_t copy_static_bytes(uint8_t *dst,
                         size_t dst_len,
                         size_t &written,
                         const uint8_t *src,
                         size_t src_len,
                         size_t &src_offset) {
    if (!dst || !src || written >= dst_len || src_offset >= src_len) return 0;
    const size_t available = dst_len - written;
    const size_t remaining = src_len - src_offset;
    const size_t n = remaining < available ? remaining : available;
    memcpy(dst + written, src + src_offset, n);
    written += n;
    src_offset += n;
    return n;
}

size_t copy_static_bytes(uint8_t *dst,
                         size_t dst_len,
                         size_t &written,
                         const char *src,
                         size_t src_len,
                         size_t &src_offset) {
    return copy_static_bytes(dst,
                             dst_len,
                             written,
                             reinterpret_cast<const uint8_t *>(src),
                             src_len,
                             src_offset);
}

}  // namespace

struct StorageArchiveJob::ArchiveEntry {
    uint32_t path_offset = 0;
    uint32_t size = 0;
    uint32_t crc = 0;
    uint32_t local_header_offset = 0;
    uint16_t mod_time = ZIP_DOS_TIME_DEFAULT;
    uint16_t mod_date = ZIP_DOS_DATE_DEFAULT;
    uint16_t path_len = 0;
    bool directory = false;
};

struct StorageArchiveJob::WalkFrame {
    char path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
    uint32_t next_index = 0;
    bool opened = false;
    File dir;
};

enum class ArchiveStreamPhase : uint8_t {
    EntryHeader,
    EntryName,
    FileData,
    DataDescriptor,
    CentralHeader,
    CentralName,
    Eocd,
    Done,
};

struct StorageArchiveDownload {
    StorageArchiveJob *job = nullptr;
    uint32_t id = 0;
    ArchiveStreamPhase phase = ArchiveStreamPhase::EntryHeader;
    size_t phase_offset = 0;
    size_t entry_index = 0;
    size_t central_index = 0;
    uint64_t output_offset = 0;
    uint64_t current_file_offset = 0;
    uint32_t current_crc = 0;
    uint64_t central_start_offset = 0;
    uint64_t central_size = 0;
    uint8_t scratch[ZIP_CENTRAL_HEADER_SIZE] = {};
    size_t scratch_len = 0;
    uint8_t *read_ahead = nullptr;
    size_t read_ahead_capacity = 0;
    size_t read_ahead_offset = 0;
    size_t read_ahead_len = 0;
    File input;
    bool input_open = false;
    bool complete = false;

    ~StorageArchiveDownload() {
        if (input_open) {
            Storage::Guard guard;
            input.close();
        }
        if (read_ahead) Memory::free(read_ahead);
    }
};

const char *storage_archive_state_name(StorageArchiveState state) {
    switch (state) {
        case StorageArchiveState::Idle: return "idle";
        case StorageArchiveState::Preparing: return "preparing";
        case StorageArchiveState::Ready: return "ready";
        case StorageArchiveState::Downloading: return "downloading";
        case StorageArchiveState::Error: return "error";
    }
    return "unknown";
}

bool storage_archive_valid_path(const char *path) {
    return storage_user_path_valid(path);
}

void StorageArchiveJob::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
}

bool StorageArchiveJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms));
}

void StorageArchiveJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageArchiveJob::set_error_locked(const char *error) {
    release_walk_stack_locked();
    release_build_buffers_locked();
    status_.state = StorageArchiveState::Error;
    copy_cstr(status_.error, sizeof(status_.error), error);
    status_.updated_ms = millis_nonzero();
    Log::logf(CAT_STORAGE, LOG_WARN, "[ARCHIVE] error=%s\n",
              status_.error[0] ? status_.error : "error");
}

void StorageArchiveJob::reset_job_locked(bool keep_status) {
    release_walk_stack_locked();
    preempt_requested_.store(false);
    release_build_buffers_locked();

    if (!keep_status) {
        status_ = StorageArchiveStatus();
        status_.state = StorageArchiveState::Idle;
        status_.updated_ms = millis_nonzero();
    }
}

void StorageArchiveJob::release_build_buffers_locked() {
    if (entries_) {
        Memory::free(entries_);
        entries_ = nullptr;
    }
    if (path_bytes_) {
        Memory::free(path_bytes_);
        path_bytes_ = nullptr;
    }
    entry_count_ = 0;
    entry_capacity_ = 0;
    path_bytes_len_ = 0;
    path_bytes_capacity_ = 0;
}

void StorageArchiveJob::close_walk_files_locked() {
    if (walk_stack_) {
        for (size_t i = 0; i < walk_depth_; ++i) {
            if (walk_stack_[i].opened) {
                walk_stack_[i].dir.close();
                walk_stack_[i].opened = false;
            }
        }
    }
}

void StorageArchiveJob::close_walk_locked() {
    close_walk_files_locked();
    walk_depth_ = 0;
}

bool StorageArchiveJob::ensure_walk_stack_locked() {
    if (walk_stack_) return true;
    walk_capacity_ = ARCHIVE_MAX_DEPTH;
    walk_stack_ = static_cast<WalkFrame *>(
        Memory::alloc_large(sizeof(WalkFrame) * walk_capacity_, false));
    if (walk_stack_) {
        for (size_t i = 0; i < walk_capacity_; ++i) {
            new (&walk_stack_[i]) WalkFrame();
        }
        return true;
    }
    walk_capacity_ = 0;
    log_archive_alloc_failed("walk_stack",
                             sizeof(WalkFrame) * ARCHIVE_MAX_DEPTH);
    return false;
}

void StorageArchiveJob::release_walk_stack_locked() {
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

void StorageArchiveJob::apply_preempt_locked() {
    if (!preempt_requested_.exchange(false)) return;
    if (status_.state == StorageArchiveState::Preparing) {
        close_walk_files_locked();
        status_.updated_ms = millis_nonzero();
    }
}

void StorageArchiveJob::cleanup_stale_temp_locked() {
    if (!Storage::mounted()) return;
    File dir;
    {
        Storage::Guard guard;
        dir = Storage::open(AC_STORAGE_ARCHIVE_TEMP_DIR, "r");
    }
    bool is_dir = false;
    {
        Storage::Guard guard;
        is_dir = dir && dir.isDirectory();
    }
    if (!dir || !is_dir) {
        if (dir) {
            Storage::Guard guard;
            dir.close();
        }
        return;
    }
    for (;;) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir, child)) break;
        char path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
        if (!child.is_dir &&
            storage_append_child_path(AC_STORAGE_ARCHIVE_TEMP_DIR,
                                      child.name,
                                      path,
                                      sizeof(path)) &&
            archive_temp_name(path)) {
            (void)Storage::remove(path);
        }
    }
    {
        Storage::Guard guard;
        dir.close();
    }
}

bool StorageArchiveJob::begin_job_locked(const char *source_path,
                                         bool recursive,
                                         const char *filename_base,
                                         char *error_out,
                                         size_t error_out_size) {
    if (status_.state == StorageArchiveState::Preparing ||
        status_.state == StorageArchiveState::Downloading) {
        copy_cstr(error_out, error_out_size, "archive_busy");
        return false;
    }
    reset_job_locked(false);

    if (!ensure_walk_stack_locked()) {
        set_error_locked("metadata_alloc");
        copy_cstr(error_out, error_out_size, status_.error);
        return false;
    }

    status_.state = StorageArchiveState::Preparing;
    status_.id = next_id_++;
    if (status_.id == 0) status_.id = next_id_++;
    status_.recursive = recursive;
    status_.psram_metadata = Memory::psram_available();
    copy_cstr(status_.source_path, sizeof(status_.source_path), source_path);
    storage_normalize_path(status_.source_path);
    const char *base =
        filename_base && *filename_base ? filename_base :
        strcmp(status_.source_path, "/") == 0
            ? "storage"
            : storage_basename_from_path(status_.source_path);
    if (!base || !*base) base = "storage";
    snprintf(status_.filename, sizeof(status_.filename), "%s.zip", base);
    status_.started_ms = millis_nonzero();
    status_.updated_ms = status_.started_ms;
    return true;
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
    if (!begin_job_locked(path,
                          recursive,
                          nullptr,
                          error_out,
                          error_out_size)) {
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

bool StorageArchiveJob::start_selected(const char *base_path,
                                       const char *const *names,
                                       size_t count,
                                       uint32_t *id_out,
                                       char *error_out,
                                       size_t error_out_size) {
    if (id_out) *id_out = 0;
    copy_cstr(error_out, error_out_size, "");
    begin();
    if (!storage_archive_valid_path(base_path)) {
        copy_cstr(error_out, error_out_size, "bad_path");
        return false;
    }
    if (!names || count == 0 || count > AC_STORAGE_ARCHIVE_MAX_SELECTIONS) {
        copy_cstr(error_out, error_out_size, "bad_selection");
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!storage_valid_child_name(names[i])) {
            copy_cstr(error_out, error_out_size, "bad_name");
            return false;
        }
    }

    if (!lock(50)) {
        copy_cstr(error_out, error_out_size, "busy");
        return false;
    }
    const char *filename_base = count == 1 ? names[0] : nullptr;
    if (!begin_job_locked(base_path,
                          true,
                          filename_base,
                          error_out,
                          error_out_size)) {
        unlock();
        return false;
    }

    {
        Storage::Guard guard;
        File base_dir = Storage::open(status_.source_path, "r");
        if (!base_dir || !base_dir.isDirectory()) {
            if (base_dir) base_dir.close();
            set_error_locked("not_directory");
            copy_cstr(error_out, error_out_size, status_.error);
            unlock();
            return false;
        }
        base_dir.close();
    }

    for (size_t i = 0; i < count; ++i) {
        char child_path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
        if (!storage_append_child_path(status_.source_path,
                               names[i],
                               child_path,
                               sizeof(child_path)) ||
            !storage_archive_valid_path(child_path)) {
            set_error_locked("bad_child_path");
            copy_cstr(error_out, error_out_size, status_.error);
            unlock();
            return false;
        }

        bool is_dir = false;
        bool child_found = false;
        uint64_t size = 0;
        time_t last_write = 0;
        {
            Storage::Guard guard;
            File child = Storage::open(child_path, "r");
            if (child) {
                child_found = true;
                is_dir = child.isDirectory();
                size = is_dir ? 0 : static_cast<uint64_t>(child.size());
                last_write = child.getLastWrite();
                child.close();
            }
        }
        if (!child_found) {
            set_error_locked("not_found");
            copy_cstr(error_out, error_out_size, status_.error);
            unlock();
            return false;
        }

        if (is_dir) {
            if (!append_entry_locked(child_path, 0, true, last_write) ||
                !push_walk_dir_locked(child_path)) {
                copy_cstr(error_out, error_out_size, status_.error);
                unlock();
                return false;
            }
        } else if (!append_entry_locked(child_path, size, false, last_write)) {
            copy_cstr(error_out, error_out_size, status_.error);
            unlock();
            return false;
        }
    }

    if (id_out) *id_out = status_.id;
    unlock();
    return true;
}

bool StorageArchiveJob::status(StorageArchiveStatus &out,
                               uint32_t timeout_ms) const {
    if (!lock(timeout_ms)) return false;
    out = status_;
    unlock();
    return true;
}

StorageArchiveStatus StorageArchiveJob::status() const {
    StorageArchiveStatus out;
    (void)status(out);
    return out;
}

bool StorageArchiveJob::active() const {
    if (!lock(20)) return true;
    const bool out =
        status_.state == StorageArchiveState::Preparing ||
        status_.state == StorageArchiveState::Downloading;
    unlock();
    return out;
}

bool StorageArchiveJob::begin_download(
    uint32_t id,
    std::shared_ptr<StorageArchiveDownload> &download_out,
    char *filename_out,
    size_t filename_out_size,
    uint64_t &size_out,
    char *error_out,
    size_t error_out_size) {
    download_out.reset();
    size_out = 0;
    copy_cstr(error_out, error_out_size, "");
    std::shared_ptr<StorageArchiveDownload> download =
        std::make_shared<StorageArchiveDownload>();
    if (!download) {
        copy_cstr(error_out, error_out_size, "download_alloc");
        return false;
    }
    download->read_ahead = static_cast<uint8_t *>(
        Memory::alloc_large(ARCHIVE_READ_AHEAD_BYTES, false));
    if (download->read_ahead) {
        download->read_ahead_capacity = ARCHIVE_READ_AHEAD_BYTES;
    } else if (Memory::psram_available()) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[ARCHIVE] read_ahead allocation failed bytes=%u\n",
                  static_cast<unsigned>(ARCHIVE_READ_AHEAD_BYTES));
    }
    if (!lock(1000)) {
        copy_cstr(error_out, error_out_size, "busy");
        return false;
    }
    if (status_.id != id || status_.state != StorageArchiveState::Ready) {
        copy_cstr(error_out, error_out_size, "not_ready");
        unlock();
        return false;
    }
    status_.state = StorageArchiveState::Downloading;
    status_.bytes_done = 0;
    status_.files_done = 0;
    status_.updated_ms = millis_nonzero();
    download->job = this;
    download->id = id;
    copy_cstr(filename_out, filename_out_size, status_.filename);
    size_out = status_.estimated_archive_bytes;
    download_out = download;
    unlock();
    return true;
}

void StorageArchiveJob::finish_download(StorageArchiveDownload &download) {
    if (download.input_open) {
        Storage::Guard guard;
        download.input.close();
        download.input_open = false;
    }
    if (!lock(1000)) return;
    if (status_.id == download.id &&
        status_.state == StorageArchiveState::Downloading) {
        if (download.complete &&
            download.output_offset == status_.estimated_archive_bytes) {
            reset_job_locked(false);
        } else {
            set_error_locked("download_aborted");
        }
    }
    unlock();
}

JobStep StorageArchiveJob::step() {
    begin();
    if (!lock(50)) return JobStep::Waiting;

    apply_preempt_locked();

    const uint32_t now = millis_nonzero();
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
    }
    apply_preempt_locked();
    unlock();
    return result;
}

void StorageArchiveJob::on_preempt() {
    preempt_requested_.store(true);
    if (!lock(20)) return;
    apply_preempt_locked();
    unlock();
}

bool StorageArchiveJob::reserve_entries_locked(size_t needed) {
    if (needed <= entry_capacity_) return true;
    size_t next = entry_capacity_ ? entry_capacity_ * 2
                                  : ARCHIVE_INITIAL_ENTRY_CAPACITY;
    while (next < needed) next *= 2;
    ArchiveEntry *entries = static_cast<ArchiveEntry *>(
        Memory::calloc_large(next, sizeof(ArchiveEntry), false));
    if (!entries) {
        log_archive_alloc_failed("metadata_entries",
                                 next * sizeof(ArchiveEntry));
        return false;
    }
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
    if (!bytes) {
        log_archive_alloc_failed("metadata_paths", next);
        return false;
    }
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
                                            bool directory,
                                            time_t last_write) {
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
    const ZipDosTimestamp timestamp = zip_dos_timestamp(last_write);
    entry.mod_time = timestamp.time;
    entry.mod_date = timestamp.date;
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
    if (!storage_skip_dir_children(frame.dir, frame.next_index)) {
        frame.dir.close();
        set_error_locked("walk_resume_failed");
        return false;
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

        StorageDirChild child;
        if (!storage_read_next_dir_child(frame.dir, child)) {
            if (frame.opened) {
                Storage::Guard guard;
                frame.dir.close();
                frame.opened = false;
            }
            walk_depth_--;
            continue;
        }
        frame.next_index++;
        budget--;

        char child_path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
        const bool path_ok = storage_append_child_path(frame.path,
                                                       child.name,
                                                       child_path,
                                                       sizeof(child_path));

        if (!path_ok) {
            set_error_locked("bad_child_path");
            return false;
        }
        if (storage_path_equals_or_under(child_path, AC_STORAGE_ARCHIVE_TEMP_DIR)) {
            continue;
        }
        if (!storage_archive_valid_path(child_path)) {
            set_error_locked("bad_child_path");
            return false;
        }
        if (child.is_dir) {
            if (status_.recursive) {
                if (!append_entry_locked(child_path, 0, true,
                                         child.last_write) ||
                    !push_walk_dir_locked(child_path)) {
                    return false;
                }
            }
            continue;
        }
        if (!append_entry_locked(child_path, child.size, false,
                                 child.last_write)) {
            return false;
        }
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
    status_.state = StorageArchiveState::Ready;
    status_.archive_bytes = status_.estimated_archive_bytes;
    status_.bytes_done = 0;
    status_.updated_ms = millis_nonzero();
    release_walk_stack_locked();
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[ARCHIVE] ready id=%lu path=%s recursive=%u files=%lu "
              "input=%llu bytes=%llu\n",
              static_cast<unsigned long>(status_.id),
              status_.source_path,
              status_.recursive ? 1u : 0u,
              static_cast<unsigned long>(status_.files),
              static_cast<unsigned long long>(status_.input_bytes),
              static_cast<unsigned long long>(status_.estimated_archive_bytes));
    return true;
}

size_t StorageArchiveJob::read_download(StorageArchiveDownload &download,
                                        uint8_t *buffer,
                                        size_t max_len,
                                        size_t offset) {
    if (!buffer || max_len == 0) return 0;
    if (!lock(1000)) return 0;
    const size_t out = read_download_locked(download, buffer, max_len, offset);
    unlock();
    return out;
}

size_t StorageArchiveJob::read_download_locked(StorageArchiveDownload &download,
                                               uint8_t *buffer,
                                               size_t max_len,
                                               size_t offset) {
    if (download.id != status_.id ||
        status_.state != StorageArchiveState::Downloading ||
        offset != download.output_offset) {
        return 0;
    }

    size_t written = 0;
    while (written < max_len && download.phase != ArchiveStreamPhase::Done) {
        if (download.phase == ArchiveStreamPhase::EntryHeader) {
            if (download.entry_index >= entry_count_) {
                download.central_start_offset = download.output_offset;
                download.central_index = 0;
                download.phase = ArchiveStreamPhase::CentralHeader;
                download.phase_offset = 0;
                continue;
            }
            ArchiveEntry &entry = entries_[download.entry_index];
            if (download.phase_offset == 0) {
                entry.local_header_offset =
                    static_cast<uint32_t>(download.output_offset);
                entry.crc = 0;
                memset(download.scratch, 0, ZIP_LOCAL_HEADER_SIZE);
                put_le32(download.scratch, 0, 0x04034b50u);
                put_le16(download.scratch, 4, 20);
                put_le16(download.scratch,
                         6,
                         entry.directory ? 0 : ZIP_GENERAL_DATA_DESCRIPTOR);
                put_le16(download.scratch, 8, ZIP_METHOD_STORE);
                put_le16(download.scratch, 10, entry.mod_time);
                put_le16(download.scratch, 12, entry.mod_date);
                put_le32(download.scratch, 14, entry.directory ? entry.crc : 0);
                put_le32(download.scratch, 18, entry.directory ? entry.size : 0);
                put_le32(download.scratch, 22, entry.directory ? entry.size : 0);
                put_le16(download.scratch, 26, entry.path_len);
                download.scratch_len = ZIP_LOCAL_HEADER_SIZE;
            }
            const size_t before = written;
            copy_static_bytes(buffer,
                              max_len,
                              written,
                              download.scratch,
                              download.scratch_len,
                              download.phase_offset);
            download.output_offset += written - before;
            if (download.phase_offset >= download.scratch_len) {
                download.phase = ArchiveStreamPhase::EntryName;
                download.phase_offset = 0;
            }
            continue;
        }

        if (download.phase == ArchiveStreamPhase::EntryName) {
            ArchiveEntry &entry = entries_[download.entry_index];
            const char *name = path_bytes_ + entry.path_offset;
            const size_t before = written;
            copy_static_bytes(buffer,
                              max_len,
                              written,
                              name,
                              entry.path_len,
                              download.phase_offset);
            download.output_offset += written - before;
            if (download.phase_offset >= entry.path_len) {
                if (entry.directory) {
                    download.entry_index++;
                    download.phase = ArchiveStreamPhase::EntryHeader;
                } else {
                    download.current_file_offset = 0;
                    download.current_crc = crc32_ieee_initial_state();
                    download.phase = ArchiveStreamPhase::FileData;
                }
                download.phase_offset = 0;
            }
            continue;
        }

        if (download.phase == ArchiveStreamPhase::FileData) {
            ArchiveEntry &entry = entries_[download.entry_index];
            if (download.current_file_offset >= entry.size) {
                if (download.input_open) {
                    Storage::Guard guard;
                    download.input.close();
                    download.input_open = false;
                }
                download.read_ahead_offset = 0;
                download.read_ahead_len = 0;
                entry.crc = crc32_ieee_finish_state(download.current_crc);
                download.phase = ArchiveStreamPhase::DataDescriptor;
                download.phase_offset = 0;
                continue;
            }
            if (!download.input_open) {
                const char *name = path_bytes_ + entry.path_offset;
                char absolute[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
                if (strcmp(status_.source_path, "/") == 0) {
                    snprintf(absolute, sizeof(absolute), "/%s", name);
                } else {
                    snprintf(absolute,
                             sizeof(absolute),
                             "%s/%s",
                             status_.source_path,
                             name);
                }
                Storage::Guard guard;
                download.input = Storage::open(absolute, "r");
                if (download.input) set_archive_file_buffer(download.input);
                if (!download.input || download.input.isDirectory()) {
                    if (download.input) download.input.close();
                    set_error_locked("input_open");
                    return written;
                }
                download.input_open = true;
            }
            const uint64_t file_remaining =
                entry.size - download.current_file_offset;
            const size_t out_remaining = max_len - written;
            if (download.read_ahead_capacity > 0) {
                if (download.read_ahead_offset >= download.read_ahead_len) {
                    const size_t want = static_cast<size_t>(
                        file_remaining < download.read_ahead_capacity
                            ? file_remaining
                            : download.read_ahead_capacity);
                    size_t read = 0;
                    {
                        Storage::Guard guard;
                        read = download.input.read(download.read_ahead, want);
                    }
                    if (read == 0) {
                        set_error_locked("input_read");
                        return written;
                    }
                    download.read_ahead_offset = 0;
                    download.read_ahead_len = read;
                }
                const size_t buffered =
                    download.read_ahead_len - download.read_ahead_offset;
                const size_t n = buffered < out_remaining ? buffered
                                                          : out_remaining;
                memcpy(buffer + written,
                       download.read_ahead + download.read_ahead_offset,
                       n);
                download.current_crc =
                    crc32_ieee_update_state(download.current_crc,
                                            buffer + written,
                                            n);
                download.read_ahead_offset += n;
                download.current_file_offset += n;
                download.output_offset += n;
                written += n;
            } else {
                const size_t want = static_cast<size_t>(
                    file_remaining < out_remaining ? file_remaining
                                                    : out_remaining);
                size_t read = 0;
                {
                    Storage::Guard guard;
                    read = download.input.read(buffer + written, want);
                }
                if (read == 0) {
                    set_error_locked("input_read");
                    return written;
                }
                download.current_crc =
                    crc32_ieee_update_state(download.current_crc,
                                            buffer + written,
                                            read);
                download.current_file_offset += read;
                download.output_offset += read;
                written += read;
            }
            continue;
        }

        if (download.phase == ArchiveStreamPhase::DataDescriptor) {
            ArchiveEntry &entry = entries_[download.entry_index];
            if (download.phase_offset == 0) {
                memset(download.scratch, 0, ZIP_DATA_DESCRIPTOR_SIZE);
                put_le32(download.scratch, 0, 0x08074b50u);
                put_le32(download.scratch, 4, entry.crc);
                put_le32(download.scratch, 8, entry.size);
                put_le32(download.scratch, 12, entry.size);
                download.scratch_len = ZIP_DATA_DESCRIPTOR_SIZE;
            }
            const size_t before = written;
            copy_static_bytes(buffer,
                              max_len,
                              written,
                              download.scratch,
                              download.scratch_len,
                              download.phase_offset);
            download.output_offset += written - before;
            if (download.phase_offset >= download.scratch_len) {
                status_.files_done++;
                download.entry_index++;
                download.phase = ArchiveStreamPhase::EntryHeader;
                download.phase_offset = 0;
            }
            continue;
        }

        if (download.phase == ArchiveStreamPhase::CentralHeader) {
            if (download.central_index >= entry_count_) {
                download.phase = ArchiveStreamPhase::Eocd;
                download.phase_offset = 0;
                continue;
            }
            const ArchiveEntry &entry = entries_[download.central_index];
            if (download.phase_offset == 0) {
                memset(download.scratch, 0, ZIP_CENTRAL_HEADER_SIZE);
                put_le32(download.scratch, 0, 0x02014b50u);
                put_le16(download.scratch, 4, 20);
                put_le16(download.scratch, 6, 20);
                put_le16(download.scratch,
                         8,
                         entry.directory ? 0 : ZIP_GENERAL_DATA_DESCRIPTOR);
                put_le16(download.scratch, 10, ZIP_METHOD_STORE);
                put_le16(download.scratch, 12, entry.mod_time);
                put_le16(download.scratch, 14, entry.mod_date);
                put_le32(download.scratch, 16, entry.crc);
                put_le32(download.scratch, 20, entry.size);
                put_le32(download.scratch, 24, entry.size);
                put_le16(download.scratch, 28, entry.path_len);
                put_le32(download.scratch, 38, entry.directory ? 0x10u : 0u);
                put_le32(download.scratch, 42, entry.local_header_offset);
                download.scratch_len = ZIP_CENTRAL_HEADER_SIZE;
            }
            const size_t before = written;
            copy_static_bytes(buffer,
                              max_len,
                              written,
                              download.scratch,
                              download.scratch_len,
                              download.phase_offset);
            download.output_offset += written - before;
            if (download.phase_offset >= download.scratch_len) {
                download.phase = ArchiveStreamPhase::CentralName;
                download.phase_offset = 0;
            }
            continue;
        }

        if (download.phase == ArchiveStreamPhase::CentralName) {
            const ArchiveEntry &entry = entries_[download.central_index];
            const char *name = path_bytes_ + entry.path_offset;
            const size_t before = written;
            copy_static_bytes(buffer,
                              max_len,
                              written,
                              name,
                              entry.path_len,
                              download.phase_offset);
            const size_t copied = written - before;
            download.output_offset += copied;
            if (download.phase_offset >= entry.path_len) {
                download.central_size += ZIP_CENTRAL_HEADER_SIZE +
                                         entry.path_len;
                download.central_index++;
                download.phase = ArchiveStreamPhase::CentralHeader;
                download.phase_offset = 0;
            }
            continue;
        }

        if (download.phase == ArchiveStreamPhase::Eocd) {
            if (download.phase_offset == 0) {
                if (entry_count_ > 0xFFFFu ||
                    download.central_start_offset > ZIP32_MAX ||
                    download.central_size > ZIP32_MAX) {
                    set_error_locked("zip32_limit");
                    return written;
                }
                memset(download.scratch, 0, ZIP_EOCD_SIZE);
                put_le32(download.scratch, 0, 0x06054b50u);
                put_le16(download.scratch, 8,
                         static_cast<uint16_t>(entry_count_));
                put_le16(download.scratch, 10,
                         static_cast<uint16_t>(entry_count_));
                put_le32(download.scratch, 12,
                         static_cast<uint32_t>(download.central_size));
                put_le32(download.scratch, 16,
                         static_cast<uint32_t>(download.central_start_offset));
                download.scratch_len = ZIP_EOCD_SIZE;
            }
            const size_t before = written;
            copy_static_bytes(buffer,
                              max_len,
                              written,
                              download.scratch,
                              download.scratch_len,
                              download.phase_offset);
            download.output_offset += written - before;
            if (download.phase_offset >= download.scratch_len) {
                download.phase = ArchiveStreamPhase::Done;
                download.complete = true;
            }
            continue;
        }
    }

    status_.bytes_done = download.output_offset;
    status_.archive_bytes = status_.estimated_archive_bytes;
    status_.updated_ms = millis_nonzero();
    return written;
}

}  // namespace aircannect
