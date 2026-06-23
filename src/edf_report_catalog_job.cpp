#include "edf_report_catalog_job.h"

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
    return true;
}

void EdfReportCatalogJob::publish_build_locked() {
    if (sessions_) Memory::free(sessions_);
    sessions_ = nullptr;
    session_count_ = 0;

    if (build_session_count_ > 0) {
        EdfReportSessionDescriptor *packed =
            static_cast<EdfReportSessionDescriptor *>(Memory::alloc_large(
                build_session_count_ * sizeof(EdfReportSessionDescriptor),
                false));
        if (packed) {
            memcpy(packed,
                   build_sessions_,
                   build_session_count_ *
                       sizeof(EdfReportSessionDescriptor));
            Memory::free(build_sessions_);
            sessions_ = packed;
        } else {
            Log::logf(CAT_EDF, LOG_WARN,
                      "report catalog compact allocation failed "
                      "sessions=%u bytes=%u; keeping full buffer\n",
                      static_cast<unsigned>(build_session_count_),
                      static_cast<unsigned>(
                          build_session_count_ *
                          sizeof(EdfReportSessionDescriptor)));
            sessions_ = build_sessions_;
        }
        session_count_ = build_session_count_;
    } else {
        Memory::free(build_sessions_);
    }
    build_sessions_ = nullptr;
    build_session_count_ = 0;

    status_.state = EdfReportCatalogState::Ready;
    status_.sessions = session_count_;
    status_.build_sessions = 0;
    status_.error[0] = '\0';
    update_current_path_locked("");
}

void EdfReportCatalogJob::update_current_path_locked(const char *path) {
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
}

bool EdfReportCatalogJob::request_refresh(uint32_t *refresh_id_out) {
    begin();
    if (!lock(0)) return false;
    if (status_.state == EdfReportCatalogState::Refreshing) {
        unlock();
        return false;
    }

    close_dirs_locked();
    release_build_locked();
    if (!allocate_build_locked()) {
        unlock();
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
    unlock();
    if (BackgroundWorker *worker = background_worker()) worker->wake();
    return true;
}

JobStep EdfReportCatalogJob::step() {
    begin();
    if (!lock(20)) return JobStep::Waiting;
    if (status_.state != EdfReportCatalogState::Refreshing) {
        unlock();
        return JobStep::Idle;
    }

    JobStep out = JobStep::Idle;
    switch (phase_) {
        case Phase::OpenRoot: out = open_root_locked(); break;
        case Phase::ReadDay: out = read_day_locked(); break;
        case Phase::OpenDay: out = open_day_locked(); break;
        case Phase::ReadFile: out = read_file_locked(); break;
        case Phase::ReadHeader: out = read_header_locked(); break;
        case Phase::Idle:
        default:
            out = JobStep::Idle;
            break;
    }
    unlock();
    return out;
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
        close_dirs_locked();
        publish_build_locked();
        phase_ = Phase::Idle;
        return JobStep::Idle;
    }
    if (!child.is_dir ||
        !storage_export_is_datalog_day_name(child.name)) {
        return JobStep::Working;
    }
    if (!storage_append_child_path("/DATALOG",
                                   child.name,
                                   day_path_,
                                   sizeof(day_path_))) {
        set_error_locked("day_path_too_long");
        return JobStep::Idle;
    }
    phase_ = Phase::OpenDay;
    update_current_path_locked(day_path_);
    return JobStep::Working;
}

JobStep EdfReportCatalogJob::open_day_locked() {
    Storage::Guard guard;
    day_dir_ = Storage::open(day_path_, "r");
    if (!day_dir_) {
        status_.files_skipped++;
        phase_ = Phase::ReadDay;
        return JobStep::Working;
    }
    if (!day_dir_.isDirectory()) {
        day_dir_.close();
        status_.files_skipped++;
        phase_ = Phase::ReadDay;
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
        phase_ = Phase::ReadDay;
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

JobStep EdfReportCatalogJob::read_header_locked() {
    uint8_t fixed_header[AC_EDF_HEADER_FIXED_SIZE] = {};
    uint8_t *header = nullptr;
    size_t header_size = 0;
    size_t file_size = 0;
    time_t last_write = current_file_mtime_;
    bool read_ok = false;
    bool alloc_failed = false;

    {
        Storage::Guard guard;
        File file = Storage::open(current_file_path_, "r");
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

    if (alloc_failed) {
        set_error_locked("alloc_failed");
    }

    if (status_.state != EdfReportCatalogState::Refreshing) {
        if (header) Memory::free(header);
        return JobStep::Idle;
    }

    if (!read_ok || !header) {
        if (header) Memory::free(header);
        status_.files_skipped++;
        phase_ = Phase::ReadFile;
        return JobStep::Working;
    }

    EdfReportFileDescriptor file;
    const EdfReportFileStatus status = edf_report_describe_file(
        current_file_path_,
        header,
        header_size,
        file_size,
        last_write,
        file);
    Memory::free(header);

    if (status == EdfReportFileStatus::Ok &&
        add_file_to_build_locked(file)) {
        status_.files_indexed++;
    } else {
        status_.files_skipped++;
    }

    phase_ = Phase::ReadFile;
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
