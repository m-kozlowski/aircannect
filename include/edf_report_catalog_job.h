#pragma once

#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "background_worker.h"
#include "edf_report_catalog.h"
#include "storage_path.h"

namespace aircannect {

static constexpr size_t AC_EDF_REPORT_CATALOG_SESSION_MAX = 192;
static constexpr size_t AC_EDF_REPORT_CATALOG_DAY_MAX = 128;

enum class EdfReportCatalogState : uint8_t {
    Idle,
    Refreshing,
    Ready,
    Error,
};

const char *edf_report_catalog_state_name(EdfReportCatalogState state);

struct EdfReportCatalogStatus {
    EdfReportCatalogState state = EdfReportCatalogState::Idle;
    uint32_t refresh_id = 0;
    uint32_t sessions = 0;
    uint32_t build_sessions = 0;
    uint32_t days_scanned = 0;
    uint32_t files_scanned = 0;
    uint32_t files_indexed = 0;
    uint32_t files_skipped = 0;
    bool truncated = false;
    char current_path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class EdfReportCatalogJob final : public BackgroundJob {
public:
    ~EdfReportCatalogJob() override;

    void begin();

    const char *name() const override { return "edf_report_catalog"; }
    JobStep step() override;
    bool run_when_gate_closed(const char *reason) const override;
    void on_preempt() override;

    bool set_timezone_offset_minutes(int32_t offset_minutes);
    bool request_refresh(uint32_t *refresh_id_out = nullptr);
    bool status(EdfReportCatalogStatus &out,
                uint32_t timeout_ms = 0) const;
    EdfReportCatalogStatus status() const;
    bool timezone_offset_minutes(int32_t &out) const;

    size_t session_count() const;
    bool copy_session(size_t index, EdfReportSessionDescriptor &out) const;

private:
    enum class Phase : uint8_t {
        Idle,
        OpenRoot,
        ReadDay,
        OpenDay,
        ReadFile,
        ReadHeader,
    };

    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void set_error_locked(const char *error);
    void close_dirs_locked();
    void release_build_locked();
    void release_build_days_locked();
    bool allocate_build_locked();
    void publish_snapshot_locked(bool final);
    void publish_partial_build_locked();
    void publish_build_locked();
    void update_current_path_locked(const char *path);
    bool start_refresh_locked(uint32_t *refresh_id_out);

    JobStep open_root_locked();
    JobStep read_day_locked();
    JobStep open_day_locked();
    JobStep read_file_locked();
    JobStep read_header_unlocked();

    bool add_file_to_build_locked(const EdfReportFileDescriptor &file);

    mutable StaticSemaphore_t lock_storage_ = {};
    mutable SemaphoreHandle_t lock_ = nullptr;
    EdfReportCatalogStatus status_;
    uint32_t next_refresh_id_ = 1;
    Phase phase_ = Phase::Idle;
    bool refresh_again_pending_ = false;

    EdfReportSessionDescriptor *sessions_ = nullptr;
    size_t session_count_ = 0;

    EdfReportSessionDescriptor *build_sessions_ = nullptr;
    size_t build_session_count_ = 0;

    struct DayName {
        char name[9] = {};
    };
    DayName *build_days_ = nullptr;
    size_t build_day_count_ = 0;
    size_t build_day_index_ = 0;

    File root_dir_;
    File day_dir_;
    char day_path_[AC_STORAGE_PATH_MAX] = {};
    char current_file_path_[AC_STORAGE_PATH_MAX] = {};
    uint64_t current_file_size_ = 0;
    time_t current_file_mtime_ = 0;
    bool timezone_offset_valid_ = false;
    int32_t timezone_offset_minutes_ = 0;
    bool timezone_refresh_pending_ = false;
};

}  // namespace aircannect
