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
    "/aircannect/report/v7/catalog.bin";

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

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port);

    OperationAdmission request_load(uint32_t generation);
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

private:
    void reset_operation();
    void fail(const char *error);

    StorageReadPort *read_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    NightCatalogStoreRuntime *runtime_ = nullptr;
    NightCatalogStoreStatus status_;
    std::shared_ptr<const NightCatalog> published_;
};

}  // namespace aircannect
