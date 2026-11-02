#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "night_catalog_file.h"
#include "operation_outcome.h"
#include "storage_atomic_write_port.h"
#include "storage_delete_port.h"
#include "storage_read_port.h"
#include "storage_range_write_port.h"
#include "storage_path_port.h"
#include "storage_scan_port.h"

namespace aircannect {

static constexpr const char *NIGHT_CATALOG_STORE_PATH =
    "/aircannect/report/v9/catalog.bin";

enum class NightCatalogStoreState : uint8_t {
    Idle,
    Loading,
    Saving,
    Ready,
    Error,
};

struct NightCatalogStoreStatus {
    NightCatalogStoreState state = NightCatalogStoreState::Idle;
    uint32_t generation = 0;
    size_t bytes = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

struct NightCatalogStoreRuntime;

class NightCatalogStoreService {
public:
    // Lifecycle and operation servicing.
    NightCatalogStoreService() = default;
    ~NightCatalogStoreService();

    NightCatalogStoreService(const NightCatalogStoreService &) = delete;
    NightCatalogStoreService &operator=(
        const NightCatalogStoreService &) = delete;

    void begin(StorageReadPort &read_port);
    void begin(StorageReadPort &read_port,
               StorageRangeWritePort &range_write_port,
               StorageAtomicWritePort &write_port,
               StorageDeletePort *delete_port = nullptr);
    bool poll(bool allow_layout_mutation = true);
    void cancel();
    void forget_media();

    // Prepare the on-card directory layout before any report readers/writers.
    void configure_layout(StorageScanPort &scans, StoragePathPort &paths);
    OperationAdmission prepare_layout(uint32_t generation);
    bool layout_ready() const;

    // Catalog reads.
    // Both load operations publish only after a complete, validated read.
    OperationAdmission request_load(uint32_t generation);
    // Use a separate loader; compare its source_revision with the index.
    OperationAdmission request_load_night(SleepDayId sleep_day,
                                          uint32_t generation,
                                          StorageReadLane lane =
                                              StorageReadLane::Report);

    // Catalog persistence.
    // Full sources are persisted first; the compact index is published last.
    OperationAdmission request_save(
        std::shared_ptr<const NightCatalog> catalog,
        uint32_t generation);

    // Full-refresh staging. Scratch files are never adopted across sessions.
    OperationAdmission begin_staging(uint32_t generation);
    OperationAdmission request_stage(
        std::shared_ptr<const NightCatalog> single_night,
        uint32_t generation);
    OperationAdmission request_install(
        std::shared_ptr<const NightCatalog> compact_index,
        uint32_t generation);

    // Published state and operation status.
    bool active() const;
    bool staging() const;
    bool installing() const;
    bool cleanup_pending() const;
    const NightCatalogStoreStatus &status() const { return status_; }
    std::shared_ptr<const NightCatalog> snapshot() const {
        return published_;
    }
    std::shared_ptr<const NightCatalog> take_snapshot() {
        std::shared_ptr<const NightCatalog> catalog;
        catalog.swap(published_);
        return catalog;
    }

private:
    // Operation lifecycle.
    void reset_operation();
    void clear_staging();
    void fail(const char *error);

    // Catalog reads.
    OperationAdmission start_load(const char *path,
                                  SleepDayId sleep_day,
                                  uint32_t generation,
                                  StorageReadLane lane);
    void finish_load(std::shared_ptr<const NightCatalog> catalog);

    // Directory migration uses storage-owned scans and non-replacing moves.
    bool poll_layout_path(StoragePathOperation operation, const char *source,
                          const char *destination, StoragePathCompletion &out);
    void finish_layout();

    // Save admission and source baseline.
    bool prepare_index_stream(
        std::shared_ptr<const NightCatalog> compact_index);
    bool sources_stored(const NightCatalogRecord &record) const;
    OperationAdmission reject_save(uint32_t generation,
                                   const char *error);

    // Staged-refresh admission.
    OperationAdmission reject_staging(uint32_t generation,
                                      const char *error);

    // Storage owners.
    StorageReadPort *read_port_ = nullptr;
    StorageRangeWritePort *range_write_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    StorageDeletePort *delete_port_ = nullptr;
    StorageScanPort *layout_scans_ = nullptr;
    StoragePathPort *layout_paths_ = nullptr;
    NightCatalogStoreRuntime *runtime_ = nullptr;
    NightCatalogStoreStatus status_;

    // Published catalog snapshots.
    std::shared_ptr<const NightCatalog> published_;
    // Night-only reads must not replace the catalog's persistence baseline.
    std::shared_ptr<const NightCatalog> stored_;
};

}  // namespace aircannect
