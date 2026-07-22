#pragma once

#include <memory>
#include <stdint.h>

#include "operation_outcome.h"
#include "report_artifact_index.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {

enum class ReportArtifactIndexRefreshState : uint8_t {
    Idle,
    Scanning,
    Reading,
    Building,
    Cancelling,
    Ready,
    Error,
};

struct ReportArtifactIndexRefreshStatus {
    ReportArtifactIndexRefreshState state =
        ReportArtifactIndexRefreshState::Idle;
    uint32_t generation = 0;
    uint32_t files_seen = 0;
    uint32_t files_indexed = 0;
    uint32_t files_skipped = 0;
    char current_path[AC_STORAGE_PATH_MAX] = {};
    char warning[AC_STORAGE_ERROR_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
};

struct ReportArtifactIndexRefreshRuntime;

class ReportArtifactIndexRefreshService {
public:
    ReportArtifactIndexRefreshService() = default;
    ~ReportArtifactIndexRefreshService();

    ReportArtifactIndexRefreshService(
        const ReportArtifactIndexRefreshService &) = delete;
    ReportArtifactIndexRefreshService &operator=(
        const ReportArtifactIndexRefreshService &) = delete;

    void begin(StorageScanPort &scan_port, StorageReadPort &read_port);

    OperationAdmission request_refresh(
        std::shared_ptr<const NightCatalog> catalog,
        uint32_t generation);
    bool poll();
    void cancel();

    bool active() const;
    const ReportArtifactIndexRefreshStatus &status() const {
        return status_;
    }
    std::shared_ptr<const ReportArtifactIndex> snapshot() const {
        return published_;
    }

private:
    void reset_transient();
    bool poll_cancel();
    void fail(const char *error);

    StorageScanPort *scan_port_ = nullptr;
    StorageReadPort *read_port_ = nullptr;
    ReportArtifactIndexRefreshRuntime *runtime_ = nullptr;
    ReportArtifactIndexRefreshStatus status_;
    std::shared_ptr<const ReportArtifactIndex> published_;
};

}  // namespace aircannect
