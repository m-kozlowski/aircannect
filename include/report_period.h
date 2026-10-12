#pragma once

#include <memory>
#include "large_text_buffer.h"
#include "report_signal_store_catalog.h"
#include "storage_bounded_file_loader.h"

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

// Worker-owned, read-only aggregation of published nights and checkpoints.
// Each poll advances at most one read, tail histogram or signal reduction.
class ReportPeriodService {
public:
    ReportPeriodService();
    ~ReportPeriodService();

    void begin(StorageReadPort &port);
    bool start(ReportPeriodKey key,
               std::shared_ptr<const NightCatalog> catalog,
               std::shared_ptr<const ReportSignalStoreCatalog> store,
               uint64_t revision);
    bool poll();
    void cancel();

    bool active() const;
    std::shared_ptr<const ReportPeriodResult> take_completed();

private:
    struct Runtime;
    std::shared_ptr<Runtime> runtime_;
    StorageBoundedFileLoader loader_;
};

uint64_t report_period_revision(ReportPeriodKey key,
                               const NightCatalog &catalog,
                               const ReportSignalStoreCatalog *store);

}  // namespace aircannect
