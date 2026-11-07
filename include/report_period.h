#pragma once

#include <memory>
#include <vector>
#include "large_allocator.h"
#include "large_text_buffer.h"
#include "report_signal_store_catalog.h"
#include "report_signal_store_metadata_loader.h"
#include "storage_bounded_file_loader.h"
#include "storage_atomic_write_port.h"

namespace aircannect {

struct NightCatalogLayoutIssues;
using ReportSessionSelection = std::vector<uint16_t, LargeAllocator<uint16_t>>;

struct ReportPeriodKey {
    ReportPeriodKey() = default;
    ReportPeriodKey(SleepDayId end_day, uint16_t day_count,
        std::shared_ptr<const ReportSessionSelection> selection = {},
        SourceRevision revision = {}, uint32_t version = 0)
        : end(end_day), days(day_count), sessions(std::move(selection)),
          source_revision(revision), generation(version) {}

    SleepDayId end;
    uint16_t days = 0;
    std::shared_ptr<const ReportSessionSelection> sessions;
    SourceRevision source_revision;
    uint32_t generation = 0;

    bool valid() const;
    SleepDayId first() const;
    bool operator==(const ReportPeriodKey &other) const {
        return end == other.end && days == other.days && source_revision == other.source_revision &&
            generation == other.generation &&
            ((!sessions && !other.sessions) ||
             (sessions && other.sessions && *sessions == *other.sessions));
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
    void publish_storage_low_space(bool low_space) { storage_low_space_ = low_space; }
    bool start(ReportPeriodKey key,
               std::shared_ptr<const NightCatalog> catalog,
               std::shared_ptr<const ReportSignalStoreCatalog> store,
               uint64_t revision,
               std::shared_ptr<const NightCatalogLayoutIssues> layout_issues = {});
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
    ReportSignalStoreMetadataLoader metadata_loader_;
    StorageBoundedFileLoader loader_;
    StorageAtomicWritePort *write_port_ = nullptr;
    OperationTicket write_ticket_;
    bool storage_low_space_ = false;
    StorageReadPort *read_port_ = nullptr;
};

uint64_t report_period_revision(ReportPeriodKey key,
                               const NightCatalog &catalog,
                               const ReportSignalStoreCatalog *store,
                               const NightCatalogLayoutIssues *layout_issues = nullptr);

}  // namespace aircannect
