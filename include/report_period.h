#pragma once

#include <memory>
#include "large_text_buffer.h"
#include "report_signal_store_catalog.h"
#include "storage_bounded_file_loader.h"
#include "storage_atomic_write_port.h"

namespace aircannect {

struct ReportPeriodKey {
    SleepDayId end;
    uint16_t days = 0;

    bool valid() const;
    SleepDayId first() const;
    bool operator==(const ReportPeriodKey &other) const {
        return end == other.end && days == other.days;
    }
};

struct ReportPeriodResult {
    ReportPeriodKey key;
    uint64_t revision = 0;
    LargeTextBuffer body;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// Worker-owned aggregation; old checkpoints acquire a derived sparse cache.
// Each poll advances at most one read, tail histogram or signal reduction.
class ReportPeriodService {
public:
    ReportPeriodService();
    ~ReportPeriodService();

    static constexpr size_t NightCacheBytes = 512 * 1024;

    void begin(StorageReadPort &port, StorageAtomicWritePort &writes);
    bool start(ReportPeriodKey key,
               std::shared_ptr<const NightCatalog> catalog,
               std::shared_ptr<const ReportSignalStoreCatalog> store,
               uint64_t revision);
    bool poll();
    void cancel();
    void clear_cache();

    bool active() const;
    std::shared_ptr<const ReportPeriodResult> take_completed();

private:
    struct Runtime;
    struct Cache;
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<Cache> cache_;
    StorageBoundedFileLoader loader_;
    StorageAtomicWritePort *write_port_ = nullptr;
    OperationTicket write_ticket_;
};

uint64_t report_period_revision(ReportPeriodKey key,
                               const NightCatalog &catalog,
                               const ReportSignalStoreCatalog *store);

}  // namespace aircannect
