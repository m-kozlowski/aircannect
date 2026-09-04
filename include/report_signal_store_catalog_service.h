#pragma once

#include <memory>
#include <stdint.h>

#include "night_catalog.h"
#include "operation_outcome.h"
#include "report_signal_store_catalog.h"
#include "storage_bounded_file_loader.h"

namespace aircannect {

enum class ReportSignalStoreCatalogLoadState : uint8_t {
    Idle,
    Loading,
    Ready,
    Failed,
    Cancelled,
};

struct ReportSignalStoreCatalogLoadStatus {
    ReportSignalStoreCatalogLoadState state =
        ReportSignalStoreCatalogLoadState::Idle;
    uint32_t generation = 0;
    size_t nights_checked = 0;
    size_t nights_loaded = 0;
    size_t nights_skipped = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const {
        return state == ReportSignalStoreCatalogLoadState::Loading;
    }
    bool terminal() const {
        return state == ReportSignalStoreCatalogLoadState::Ready ||
               state == ReportSignalStoreCatalogLoadState::Failed ||
               state == ReportSignalStoreCatalogLoadState::Cancelled;
    }
};

class ReportSignalStoreCatalogLoadService {
public:
    ReportSignalStoreCatalogLoadService() = default;
    ~ReportSignalStoreCatalogLoadService();

    ReportSignalStoreCatalogLoadService(
        const ReportSignalStoreCatalogLoadService &) = delete;
    ReportSignalStoreCatalogLoadService &operator=(
        const ReportSignalStoreCatalogLoadService &) = delete;

    void begin(StorageReadPort &read_port);
    OperationAdmission start(std::shared_ptr<const NightCatalog> source,
                             uint32_t generation);
    bool poll();
    void cancel();
    void reset();

    const ReportSignalStoreCatalogLoadStatus &status() const {
        return status_;
    }
    std::shared_ptr<const ReportSignalStoreCatalog> take_completed();

private:
    struct Runtime;

    bool start_current();
    bool finish_current();
    bool finish_catalog();
    void fail(const char *error);

    StorageBoundedFileLoader loader_;
    Runtime *runtime_ = nullptr;
    ReportSignalStoreCatalogLoadStatus status_;
};

}  // namespace aircannect
