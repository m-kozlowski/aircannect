#include "edf_report_catalog_job.h"

#include <algorithm>
#include <string.h>

#include "debug_log.h"
#include "edf_file_inventory.h"
#include "edf_file_reader.h"
#include "edf_layout.h"
#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_export_plan.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t EDF_REPORT_CATALOG_HEADER_MAX = 8192;

bool same_session(const EdfReportSessionDescriptor &session,
                  const EdfReportFileDescriptor &file) {
    return strcmp(session.sleep_day, file.inventory.sleep_day) == 0 &&
           strcmp(session.session_stamp, file.inventory.session_stamp) == 0;
}

}  // namespace

const char *edf_report_catalog_state_name(EdfReportCatalogState state) {
    switch (state) {
        case EdfReportCatalogState::Idle: return "idle";
        case EdfReportCatalogState::Refreshing: return "refreshing";
        case EdfReportCatalogState::Ready: return "ready";
        case EdfReportCatalogState::Error: return "error";
        default:
            return "unknown";
    }
}

EdfReportCatalogJob::~EdfReportCatalogJob() {
    close_dirs_locked();
    release_build_locked();
    if (sessions_) Memory::free(sessions_);
    sessions_ = nullptr;
    session_count_ = 0;
}

void EdfReportCatalogJob::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutexStatic(&lock_storage_);
}

bool EdfReportCatalogJob::lock(uint32_t timeout_ms) const {
    if (!lock_) return false;
    return xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void EdfReportCatalogJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void EdfReportCatalogJob::set_error_locked(const char *error) {
    close_dirs_locked();
    release_build_locked();
    refresh_again_pending_ = false;
    status_.state = EdfReportCatalogState::Error;
    phase_ = Phase::Idle;
    copy_cstr(status_.error, sizeof(status_.error), error ? error : "error");
    update_current_path_locked("");
}

void EdfReportCatalogJob::close_dirs_locked() {
    if (root_dir_) {
        Storage::Guard guard;
        root_dir_.close();
    }
    root_dir_ = File();
    if (day_dir_) {
        Storage::Guard guard;
        day_dir_.close();
    }
    day_dir_ = File();
}

void EdfReportCatalogJob::release_build_locked() {
    if (build_sessions_) Memory::free(build_sessions_);
    build_sessions_ = nullptr;
    build_session_count_ = 0;
    status_.build_sessions = 0;
    release_build_days_locked();
}

void EdfReportCatalogJob::release_build_days_locked() {
    if (build_days_) Memory::free(build_days_);
    build_days_ = nullptr;
    build_day_count_ = 0;
    build_day_index_ = 0;
}

bool EdfReportCatalogJob::allocate_build_locked() {
    release_build_locked();
    build_sessions_ = static_cast<EdfReportSessionDescriptor *>(
        Memory::calloc_large(AC_EDF_REPORT_CATALOG_SESSION_MAX,
                             sizeof(EdfReportSessionDescriptor),
                             false));
    if (!build_sessions_) {
        set_error_locked("alloc_failed");
        Log::logf(CAT_EDF, LOG_ERROR,
                  "report catalog allocation failed sessions=%u bytes=%u\n",
                  static_cast<unsigned>(AC_EDF_REPORT_CATALOG_SESSION_MAX),
                  static_cast<unsigned>(AC_EDF_REPORT_CATALOG_SESSION_MAX *
                                        sizeof(EdfReportSessionDescriptor)));
        return false;
    }
    build_days_ = static_cast<DayName *>(Memory::calloc_large(
        AC_EDF_REPORT_CATALOG_DAY_MAX,
        sizeof(DayName),
        false));
    if (!build_days_) {
        Memory::free(build_sessions_);
        build_sessions_ = nullptr;
        set_error_locked("alloc_failed");
        Log::logf(CAT_EDF, LOG_ERROR,
                  "report catalog allocation failed days=%u bytes=%u\n",
                  static_cast<unsigned>(AC_EDF_REPORT_CATALOG_DAY_MAX),
                  static_cast<unsigned>(AC_EDF_REPORT_CATALOG_DAY_MAX *
                                        sizeof(DayName)));
        return false;
    }
    return true;
}

void EdfReportCatalogJob::publish_snapshot_locked(bool final) {
    EdfReportSessionDescriptor *packed = nullptr;
    size_t packed_count = 0;

    if (build_session_count_ > 0) {
        packed = static_cast<EdfReportSessionDescriptor *>(Memory::alloc_large(
            build_session_count_ * sizeof(EdfReportSessionDescriptor),
            false));
        if (packed) {
            memcpy(packed,
                   build_sessions_,
                   build_session_count_ *
                       sizeof(EdfReportSessionDescriptor));
            packed_count = build_session_count_;
        } else {
            Log::logf(CAT_EDF, LOG_WARN,
                      "report catalog compact allocation failed "
                      "sessions=%u bytes=%u; keeping full buffer\n",
                      static_cast<unsigned>(build_session_count_),
                      static_cast<unsigned>(
                          build_session_count_ *
                          sizeof(EdfReportSessionDescriptor)));
            if (final) {
                packed = build_sessions_;
                packed_count = build_session_count_;
            }
        }
    }

    if (packed || final) {
        if (sessions_ && sessions_ != packed) Memory::free(sessions_);
        sessions_ = packed;
        session_count_ = packed_count;
    }

    if (final) {
        if (build_sessions_ && build_sessions_ != sessions_) {
            Memory::free(build_sessions_);
        }
        build_sessions_ = nullptr;
        build_session_count_ = 0;
        release_build_days_locked();
        status_.state = EdfReportCatalogState::Ready;
        status_.build_sessions = 0;
        status_.error[0] = '\0';
        update_current_path_locked("");
    }
    status_.sessions = session_count_;
}

void EdfReportCatalogJob::publish_partial_build_locked() {
    // Keep the public session snapshot stable until a refresh completes.
    // Report ETags and source planning depend on this snapshot; publishing a
    // partial DATALOG walk makes the same night oscillate between identities.
    status_.sessions = session_count_;
    status_.build_sessions = build_session_count_;
}

void EdfReportCatalogJob::publish_build_locked() {
    publish_snapshot_locked(true);
    if (!refresh_again_pending_) return;
    refresh_again_pending_ = false;
    (void)start_refresh_locked(nullptr);
}

void EdfReportCatalogJob::update_current_path_locked(const char *path) {
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
}

bool EdfReportCatalogJob::start_refresh_locked(uint32_t *refresh_id_out) {
    if (status_.state == EdfReportCatalogState::Refreshing) {
        return false;
    }

    refresh_again_pending_ = false;
    close_dirs_locked();
    release_build_locked();
    if (!allocate_build_locked()) {
        return false;
    }

    status_ = EdfReportCatalogStatus();
    status_.state = EdfReportCatalogState::Refreshing;
    status_.refresh_id = next_refresh_id_++;
    if (next_refresh_id_ == 0) next_refresh_id_ = 1;
    status_.sessions = session_count_;
    phase_ = Phase::OpenRoot;
    day_path_[0] = '\0';
    current_file_path_[0] = '\0';
    current_file_size_ = 0;
    current_file_mtime_ = 0;
    if (refresh_id_out) *refresh_id_out = status_.refresh_id;
    return true;
}

bool EdfReportCatalogJob::request_refresh(uint32_t *refresh_id_out) {
    begin();
    if (!lock(0)) return false;
    bool accepted = false;
    bool started = false;
    if (status_.state == EdfReportCatalogState::Refreshing) {
        refresh_again_pending_ = true;
        if (refresh_id_out) *refresh_id_out = status_.refresh_id;
        accepted = true;
    } else {
        started = start_refresh_locked(refresh_id_out);
        accepted = started;
    }
    unlock();
    if (accepted) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return accepted;
}

bool EdfReportCatalogJob::set_timezone_offset_minutes(int32_t offset_minutes) {
    begin();
    if (!lock(0)) return false;
    const bool changed = !timezone_offset_valid_ ||
                         timezone_offset_minutes_ != offset_minutes;
    if (changed) {
        timezone_offset_valid_ = true;
        timezone_offset_minutes_ = offset_minutes;
        timezone_refresh_pending_ = true;
        if (status_.state == EdfReportCatalogState::Refreshing) {
            close_dirs_locked();
            release_build_locked();
            status_.state = session_count_ > 0 ? EdfReportCatalogState::Ready
                                               : EdfReportCatalogState::Idle;
            status_.sessions = session_count_;
            phase_ = Phase::Idle;
            update_current_path_locked("");
        }
    }
    bool started = false;
    if (timezone_refresh_pending_ &&
        status_.state != EdfReportCatalogState::Refreshing) {
        started = start_refresh_locked(nullptr);
        if (started) timezone_refresh_pending_ = false;
    }
    unlock();
    if (started) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return changed;
}

JobStep EdfReportCatalogJob::step() {
    begin();
    if (!lock(20)) return JobStep::Waiting;
    if (status_.state != EdfReportCatalogState::Refreshing) {
        if (refresh_again_pending_) {
            refresh_again_pending_ = false;
            const bool started = start_refresh_locked(nullptr);
            unlock();
            return started ? JobStep::Working : JobStep::Idle;
        }
        unlock();
        return JobStep::Idle;
    }

    JobStep out = JobStep::Idle;
    if (phase_ == Phase::ReadHeader) {
        unlock();
        return read_header_unlocked();
    }
    switch (phase_) {
        case Phase::OpenRoot: out = open_root_locked(); break;
        case Phase::ReadDay: out = read_day_locked(); break;
        case Phase::OpenDay: out = open_day_locked(); break;
        case Phase::ReadFile: out = read_file_locked(); break;
        case Phase::ReadHeader: break;
        case Phase::Idle:
        default:
            out = JobStep::Idle;
            break;
    }
    unlock();
    return out;
}

bool EdfReportCatalogJob::run_when_gate_closed(const char *reason) const {
    // Report result polling creates short web-grace windows. The catalog is a
    // bounded metadata scan and is a dependency of foreground report builds, so
    // letting it progress here avoids a self-deadlock where polling a report
    // prevents the catalog needed by that report from becoming ready.
    return reason && strcmp(reason, "web_grace") == 0;
}

void EdfReportCatalogJob::on_preempt() {
    if (!lock(0)) return;
    if (status_.state == EdfReportCatalogState::Refreshing) {
        close_dirs_locked();
        release_build_locked();
        status_.state = session_count_ > 0 ? EdfReportCatalogState::Ready
                                           : EdfReportCatalogState::Idle;
        status_.sessions = session_count_;
        phase_ = Phase::Idle;
        update_current_path_locked("");
    }
    unlock();
}

bool EdfReportCatalogJob::status(EdfReportCatalogStatus &out,
                                 uint32_t timeout_ms) const {
    if (!lock(timeout_ms)) return false;
    out = status_;
    out.sessions = session_count_;
    out.build_sessions = build_session_count_;
    unlock();
    return true;
}

EdfReportCatalogStatus EdfReportCatalogJob::status() const {
    EdfReportCatalogStatus out;
    (void)status(out);
    return out;
}

bool EdfReportCatalogJob::timezone_offset_minutes(int32_t &out) const {
    if (!lock(20)) return false;
    const bool valid = timezone_offset_valid_;
    if (valid) out = timezone_offset_minutes_;
    unlock();
    return valid;
}

size_t EdfReportCatalogJob::session_count() const {
    if (!lock(0)) return 0;
    const size_t out = session_count_;
    unlock();
    return out;
}

bool EdfReportCatalogJob::copy_session(
    size_t index,
    EdfReportSessionDescriptor &out) const {
    if (!lock(0)) return false;
    if (index >= session_count_ || !sessions_) {
        unlock();
        return false;
    }
    out = sessions_[index];
    unlock();
    return true;
}

JobStep EdfReportCatalogJob::open_root_locked() {
    if (!Storage::mounted()) {
        set_error_locked("storage_unavailable");
        return JobStep::Idle;
    }
    Storage::Guard guard;
    root_dir_ = Storage::open("/DATALOG", "r");
    if (!root_dir_) {
        publish_build_locked();
        phase_ = Phase::Idle;
        return JobStep::Idle;
    }
    if (!root_dir_.isDirectory()) {
        set_error_locked("bad_datalog");
        return JobStep::Idle;
    }
    phase_ = Phase::ReadDay;
    update_current_path_locked("/DATALOG");
    return JobStep::Working;
}

JobStep EdfReportCatalogJob::read_day_locked() {
    StorageDirChild child;
    if (!storage_read_next_dir_child(root_dir_, child)) {
        {
            Storage::Guard guard;
            root_dir_.close();
        }
        root_dir_ = File();
        if (build_day_count_ == 0) {
            publish_build_locked();
            phase_ = Phase::Idle;
            return JobStep::Idle;
        }
        std::sort(build_days_,
                  build_days_ + build_day_count_,
                  [](const DayName &a, const DayName &b) {
                      return strcmp(a.name, b.name) > 0;
                  });
        build_day_index_ = 0;
        phase_ = Phase::OpenDay;
        update_current_path_locked("/DATALOG");
        return JobStep::Working;
    }
    if (!child.is_dir ||
        !storage_export_is_datalog_day_name(child.name)) {
        return JobStep::Working;
    }
    if (build_day_count_ >= AC_EDF_REPORT_CATALOG_DAY_MAX) {
        status_.truncated = true;
        return JobStep::Working;
    }
    copy_cstr(build_days_[build_day_count_].name,
              sizeof(build_days_[build_day_count_].name),
              child.name);
    build_day_count_++;
    return JobStep::Working;
}

JobStep EdfReportCatalogJob::open_day_locked() {
    if (build_day_index_ >= build_day_count_) {
        close_dirs_locked();
        publish_build_locked();
        phase_ = Phase::Idle;
        return JobStep::Idle;
    }
    if (!storage_append_child_path("/DATALOG",
                                   build_days_[build_day_index_].name,
                                   day_path_,
                                   sizeof(day_path_))) {
        set_error_locked("day_path_too_long");
        return JobStep::Idle;
    }
    update_current_path_locked(day_path_);

    Storage::Guard guard;
    day_dir_ = Storage::open(day_path_, "r");
    if (!day_dir_) {
        status_.files_skipped++;
        build_day_index_++;
        return JobStep::Working;
    }
    if (!day_dir_.isDirectory()) {
        day_dir_.close();
        status_.files_skipped++;
        day_dir_ = File();
        build_day_index_++;
        return JobStep::Working;
    }
    status_.days_scanned++;
    phase_ = Phase::ReadFile;
    return JobStep::Working;
}

JobStep EdfReportCatalogJob::read_file_locked() {
    StorageDirChild child;
    if (!storage_read_next_dir_child(day_dir_, child)) {
        Storage::Guard guard;
        day_dir_.close();
        day_dir_ = File();
        publish_partial_build_locked();
        build_day_index_++;
        phase_ = Phase::OpenDay;
        return JobStep::Working;
    }
    if (child.is_dir) return JobStep::Working;
    if (!storage_append_child_path(day_path_,
                                   child.name,
                                   current_file_path_,
                                   sizeof(current_file_path_))) {
        set_error_locked("file_path_too_long");
        return JobStep::Idle;
    }

    EdfInventoryEntry path_entry;
    if (!edf_inventory_describe_path(current_file_path_, path_entry) ||
        !edf_report_file_kind_supported(path_entry.kind)) {
        return JobStep::Working;
    }

    current_file_size_ = child.size;
    current_file_mtime_ = child.last_write;
    status_.files_scanned++;
    phase_ = Phase::ReadHeader;
    update_current_path_locked(current_file_path_);
    return JobStep::Working;
}

JobStep EdfReportCatalogJob::read_header_unlocked() {
    char path[AC_STORAGE_PATH_MAX] = {};
    time_t snapshot_mtime = 0;
    int32_t snapshot_tz_offset_min = 0;
    uint32_t snapshot_refresh_id = 0;

    if (!lock(20)) return JobStep::Waiting;
    if (status_.state != EdfReportCatalogState::Refreshing ||
        phase_ != Phase::ReadHeader) {
        unlock();
        return JobStep::Idle;
    }
    snapshot_refresh_id = status_.refresh_id;
    snapshot_mtime = current_file_mtime_;
    snapshot_tz_offset_min = timezone_offset_minutes_;
    copy_cstr(path, sizeof(path), current_file_path_);
    unlock();

    uint8_t fixed_header[AC_EDF_HEADER_FIXED_SIZE] = {};
    uint8_t *header = nullptr;
    size_t header_size = 0;
    size_t file_size = 0;
    time_t last_write = snapshot_mtime;
    bool read_ok = false;
    bool alloc_failed = false;

    {
        Storage::Guard guard;
        File file = Storage::open(path, "r");
        if (file && !file.isDirectory()) {
            file_size = static_cast<size_t>(file.size());
            last_write = file.getLastWrite();
            if (file_size >= sizeof(fixed_header) &&
                file.read(fixed_header, sizeof(fixed_header)) ==
                    sizeof(fixed_header)) {
                uint32_t declared_size = 0;
                if (edf_parse_header_declared_size(fixed_header,
                                                   sizeof(fixed_header),
                                                   declared_size) &&
                    declared_size <= EDF_REPORT_CATALOG_HEADER_MAX &&
                    declared_size <= file_size) {
                    header_size = declared_size;
                    header = static_cast<uint8_t *>(
                        Memory::alloc_large(header_size, false));
                    if (header) {
                        memcpy(header, fixed_header, sizeof(fixed_header));
                        const size_t rest = header_size - sizeof(fixed_header);
                        read_ok = rest == 0 ||
                                  file.read(header + sizeof(fixed_header),
                                            rest) == rest;
                    } else {
                        alloc_failed = true;
                    }
                }
            }
            file.close();
        }
    }

    EdfReportFileDescriptor file;
    EdfReportFileStatus file_status = EdfReportFileStatus::InventoryError;
    if (read_ok && header) {
        file_status = edf_report_describe_file(path,
                                               header,
                                               header_size,
                                               file_size,
                                               last_write,
                                               snapshot_tz_offset_min,
                                               file);
    }
    if (header) Memory::free(header);

    if (!lock(20)) return JobStep::Waiting;
    if (status_.state != EdfReportCatalogState::Refreshing ||
        status_.refresh_id != snapshot_refresh_id ||
        phase_ != Phase::ReadHeader ||
        strcmp(current_file_path_, path) != 0) {
        unlock();
        return JobStep::Idle;
    }

    if (alloc_failed) {
        set_error_locked("alloc_failed");
        unlock();
        return JobStep::Idle;
    }

    if (!read_ok || file_status != EdfReportFileStatus::Ok) {
        status_.files_skipped++;
        phase_ = Phase::ReadFile;
        unlock();
        return JobStep::Working;
    }

    if (add_file_to_build_locked(file)) {
        status_.files_indexed++;
    } else {
        status_.files_skipped++;
    }

    phase_ = Phase::ReadFile;
    unlock();
    return JobStep::Working;
}

bool EdfReportCatalogJob::add_file_to_build_locked(
    const EdfReportFileDescriptor &file) {
    if (!build_sessions_) return false;
    for (size_t i = 0; i < build_session_count_; ++i) {
        if (!same_session(build_sessions_[i], file)) continue;
        return edf_report_session_add_file(build_sessions_[i], file);
    }
    if (build_session_count_ >= AC_EDF_REPORT_CATALOG_SESSION_MAX) {
        status_.truncated = true;
        return false;
    }
    EdfReportSessionDescriptor &session =
        build_sessions_[build_session_count_];
    edf_report_session_init(session);
    if (!edf_report_session_add_file(session, file)) return false;
    build_session_count_++;
    status_.build_sessions = build_session_count_;
    return true;
}

}  // namespace aircannect
