#pragma once

#include <memory>
#include <vector>

#include "large_allocator.h"
#include "report_signal_store_catalog.h"
#include "storage_bounded_file_loader.h"
#include "storage_delete_port.h"
#include "storage_scan_port.h"
#include "report_signal_store_service.h"
#include "storage_manager.h"

namespace aircannect {

struct NightCatalogLayoutIssues;

// Report-worker-only housekeeping. Readers retain the catalog from which
// their paths were obtained, including while HTTP is waiting for storage.
class ReportGenerationCleanup {
public:
    ~ReportGenerationCleanup();

    void begin(StorageReadPort &reads, StorageScanPort &scans,
               StorageDeletePort &deletes, StorageAtomicWritePort *writes = nullptr);
    void publish(std::shared_ptr<const ReportSignalStoreCatalog> catalog);
    void publish_layout(std::shared_ptr<const NightCatalogLayoutIssues> issues);
    bool poll(bool allowed, uint32_t now_ms, StorageSpaceSnapshot space = {});
    ReportSignalStoreMetadata take_published_metadata();

    // Reader-deferred nights do not hold up new background work.
    bool pending() const;
    bool pending_generations() const;
    bool waiting_for_storage() const {
        return scan_ticket_.valid() || metadata_.status().active() ||
               publication_.status().active() || delete_id_ != 0;
    }

    // The writer must not reuse an orphan path until storage has stopped
    // deleting it, even after requesting cancellation.
    bool deleting() const { return delete_id_ != 0 || reclaim_phase_ != ReclaimPhase::Idle; }
    bool forget_media();

private:
    using Retired = std::weak_ptr<const ReportSignalStoreCatalog>;
    enum class ReclaimPhase { Idle, Metadata, Pin, Scan, Verify, Publish, Delete };

    void release_readers();
    bool protected_generation(uint32_t generation) const;

    bool discard_scan();
    void finish_night(const char *error = nullptr);
    bool poll_reclaim(uint32_t now_ms);
    void finish_reclaim(const char *error = nullptr);

    // Storage owners
    StorageScanPort *scans_ = nullptr;
    StorageDeletePort *deletes_ = nullptr;
    StorageBoundedFileLoader metadata_;
    ReportSignalStoreService publication_;
    bool can_publish_ = false;

    // Published generations and reader lifetimes
    std::shared_ptr<const ReportSignalStoreCatalog> catalog_;
    std::shared_ptr<const NightCatalogLayoutIssues> layout_issues_;
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
    uint32_t pinned_generation_ = 0;
    bool pin_checked_ = false;
    char base_path_[AC_STORAGE_PATH_MAX] = {};

    // Current scan and deletion
    OperationTicket scan_ticket_;
    std::shared_ptr<const StorageScanSnapshot> entries_;
    size_t entry_cursor_ = 0;
    uint32_t operation_generation_ = 0;
    uint32_t delete_id_ = 0;
    uint32_t retry_at_ms_ = 0;

    // Emergency HTTP-cache reclamation, oldest first, after orphan cleanup.
    bool reclaim_requested_ = false;
    size_t reclaim_cursor_ = 0;
    ReclaimPhase reclaim_phase_ = ReclaimPhase::Idle;
    ReportSignalStoreMetadata reclaim_metadata_;
    size_t verify_track_ = 0;
};

}  // namespace aircannect
