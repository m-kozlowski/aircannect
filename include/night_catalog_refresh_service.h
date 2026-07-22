#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "night_catalog_builder.h"
#include "operation_outcome.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {

enum class NightCatalogRefreshState : uint8_t {
    Idle,
    Scanning,
    ReadingEdf,
    ReadingFallback,
    ReadingStr,
    Building,
    Ready,
    Error,
};

struct NightCatalogRefreshStatus {
    NightCatalogRefreshState state = NightCatalogRefreshState::Idle;
    uint32_t generation = 0;
    uint32_t files_seen = 0;
    uint32_t files_indexed = 0;
    uint32_t files_skipped = 0;
    uint32_t sessions = 0;
    uint32_t str_records = 0;
    char current_path[AC_STORAGE_PATH_MAX] = {};
    char warning[AC_STORAGE_ERROR_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
};

struct NightCatalogRefreshRuntime;

class NightCatalogRefreshService {
public:
    NightCatalogRefreshService() = default;
    ~NightCatalogRefreshService();

    NightCatalogRefreshService(const NightCatalogRefreshService &) = delete;
    NightCatalogRefreshService &operator=(
        const NightCatalogRefreshService &) = delete;

    void begin(StorageScanPort &scan_port, StorageReadPort &read_port);

    OperationAdmission request_refresh(
        const NightCatalogSummaryInput *summary_records,
        size_t summary_record_count,
        bool current_offset_valid,
        int32_t current_offset_minutes,
        uint32_t generation);
    bool poll();
    void cancel();

    bool active() const;
    const NightCatalogRefreshStatus &status() const { return status_; }
    std::shared_ptr<const NightCatalog> snapshot() const {
        return published_;
    }

private:
    void reset_transient();
    void fail(const char *error);

    StorageScanPort *scan_port_ = nullptr;
    StorageReadPort *read_port_ = nullptr;
    NightCatalogRefreshRuntime *runtime_ = nullptr;
    NightCatalogRefreshStatus status_;
    std::shared_ptr<const NightCatalog> published_;
};

}  // namespace aircannect
