#pragma once

#include "report_oximetry_signals.h"
#include "storage_delete_port.h"
#include "storage_path_port.h"

namespace aircannect {

// Oximetry pass of report generation cleanup. Never enumerate the contents
// of unrelated CPAP generations or delete a path in the originals tree.
class ReportOximetryCleanup {
public:
    void begin(ReportOximetryArchive &archive, StorageScanPort &scans,
               StorageReadPort &reads, StorageRangeWritePort &ranges,
               StorageAtomicWritePort &writes, StorageDeletePort &deletes, StoragePathPort &paths);
    bool poll(bool allowed, bool pressure);
    bool active() const { return scan_.valid() || entries_ || delete_id_; }
    bool deleting() const { return delete_id_ != 0 || remove_directory_.valid(); }
    void forget_media();

private:
    enum class Phase { Select, Contents, Verify, Delete, EmptyDirectory };
    void next();
    bool cancel();

    ReportOximetryArchive *archive_ = nullptr;
    StorageScanPort *scans_ = nullptr;
    StorageDeletePort *deletes_ = nullptr;
    StoragePathPort *paths_ = nullptr;
    ReportOximetrySignalBuilder verifier_;

    std::shared_ptr<const ReportOximetryCatalog> originals_;
    std::shared_ptr<const StorageScanSnapshot> entries_, contents_;
    OperationTicket scan_, remove_directory_;
    size_t cursor_ = 0;
    uint32_t delete_id_ = 0;
    bool due_ = false, pressure_ = false, current_ = false, manifest_ = false;
    Phase phase_ = Phase::Select;
    std::string directory_;
    ReportOximetryRecording original_;
};

}  // namespace aircannect
