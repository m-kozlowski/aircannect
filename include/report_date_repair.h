#pragma once

#include <memory>
#include "night_catalog.h"
#include "storage_atomic_write_port.h"
#include "storage_range_write_port.h"
#include "storage_scan_port.h"
#include "storage_read_port.h"
#include "storage_delete_port.h"

namespace aircannect {

enum class ReportDateRepairState : uint8_t {
    Idle, CheckingJournal, Scanning, Planning, Preview, SavingPlan,
    BackingUp, Installing, Complete, Failed
};

struct ReportDateRepairStatus {
    ReportDateRepairState state = ReportDateRepairState::Idle;
    uint32_t candidates = 0;
    uint32_t restored = 0;
    uint32_t archived = 0;
    uint32_t completed_files = 0;
    bool durable = false;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// Explicit AS11 date repair. ReportTask serializes it with all report writers.
// A durable plan resumes before report discovery or generation cleanup.
class ReportDateRepairService {
public:
    ReportDateRepairService();
    ~ReportDateRepairService();
    void begin(StorageReadPort &, StorageAtomicWritePort &,
               StorageRangeWritePort &, StorageScanPort &, StorageDeletePort &);
    bool start(std::shared_ptr<const NightCatalog> catalog);
    bool apply();
    bool poll();
    bool active() const;
    const ReportDateRepairStatus &status() const;
    std::shared_ptr<const LargeByteBuffer> preview() const;
    void cancel();

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace aircannect
