#pragma once

#include <memory>
#include <vector>

#include "large_allocator.h"
#include "report_signal_store_catalog.h"
#include "storage_bounded_file_loader.h"
#include "storage_delete_port.h"
#include "storage_scan_port.h"

namespace aircannect {

// Report-worker-only housekeeping. Readers retain the catalog from which
// their paths were obtained, including while HTTP is waiting for storage.
class ReportGenerationCleanup {
public:
    ~ReportGenerationCleanup();

    void begin(StorageReadPort &reads, StorageScanPort &scans,
               StorageDeletePort &deletes);
    void publish(std::shared_ptr<const ReportSignalStoreCatalog> catalog);
    bool poll(bool allowed, uint32_t now_ms);

    // The writer must not reuse an orphan path until storage has stopped
    // deleting it, even after requesting cancellation.
    bool deleting() const { return delete_id_ != 0; }

private:
    using Retired = std::weak_ptr<const ReportSignalStoreCatalog>;

    void release_readers();
    bool protected_generation(uint32_t generation) const;

    bool discard_scan();
    void finish_night(const char *error = nullptr);

    // Storage owners
    StorageScanPort *scans_ = nullptr;
    StorageDeletePort *deletes_ = nullptr;
    StorageBoundedFileLoader metadata_;

    // Published generations and reader lifetimes
    std::shared_ptr<const ReportSignalStoreCatalog> catalog_;
    std::vector<Retired, LargeAllocator<Retired>> retired_;
    bool tracking_failed_ = false;

    // Nights awaiting cleanup, including those deferred for readers
    std::vector<SleepDayId, LargeAllocator<SleepDayId>> pending_;
    std::vector<SleepDayId, LargeAllocator<SleepDayId>> deferred_;
    bool restart_ = false;
    bool waiting_for_readers_ = false;
    size_t night_cursor_ = 0;
    SleepDayId day_;
    uint32_t disk_generation_ = 0;
    char base_path_[AC_STORAGE_PATH_MAX] = {};

    // Current scan and deletion
    OperationTicket scan_ticket_;
    std::shared_ptr<const StorageScanSnapshot> entries_;
    size_t entry_cursor_ = 0;
    uint32_t operation_generation_ = 0;
    uint32_t delete_id_ = 0;
    uint32_t retry_at_ms_ = 0;
};

}  // namespace aircannect
