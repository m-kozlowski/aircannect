#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "night_catalog_file.h"
#include "operation_outcome.h"
#include "storage_atomic_write_port.h"
#include "storage_read_port.h"

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
    NightCatalogStoreService() = default;
    ~NightCatalogStoreService();

    NightCatalogStoreService(const NightCatalogStoreService &) = delete;
    NightCatalogStoreService &operator=(
        const NightCatalogStoreService &) = delete;

    void begin(StorageReadPort &read_port);
    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port);

    // Both load operations publish only after a complete, validated read.
    OperationAdmission request_load(uint32_t generation);
    // Use a separate loader; compare its source_revision with the index.
    OperationAdmission request_load_night(SleepDayId sleep_day,
                                          uint32_t generation,
                                          StorageReadLane lane =
                                              StorageReadLane::Report);

    // Full sources are persisted first; the compact index is published last.
    OperationAdmission request_save(
        std::shared_ptr<const NightCatalog> catalog,
        uint32_t generation);

    bool poll();
    void cancel();

    bool active() const;
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
    void reset_operation();
    void fail(const char *error);

    OperationAdmission start_load(const char *path,
                                  SleepDayId sleep_day,
                                  uint32_t generation,
                                  StorageReadLane lane);
    void finish_load(std::shared_ptr<const NightCatalog> catalog);

    bool sources_stored(const NightCatalogRecord &record) const;
    OperationAdmission reject_save(uint32_t generation,
                                   const char *error);

    StorageReadPort *read_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    NightCatalogStoreRuntime *runtime_ = nullptr;
    NightCatalogStoreStatus status_;

    std::shared_ptr<const NightCatalog> published_;
    // Night-only reads must not replace the catalog's persistence baseline.
    std::shared_ptr<const NightCatalog> stored_;
};

}  // namespace aircannect
