#pragma once

#include "report_signal_store.h"
#include "storage_bounded_file_loader.h"

namespace aircannect {

class ReportSignalStoreMetadataLoader {
public:
    ReportSignalStoreMetadataLoader() = default;
    ~ReportSignalStoreMetadataLoader() = default;

    ReportSignalStoreMetadataLoader(
        const ReportSignalStoreMetadataLoader &) = delete;
    ReportSignalStoreMetadataLoader &operator=(
        const ReportSignalStoreMetadataLoader &) = delete;

    void begin(StorageReadPort &read_port);
    OperationAdmission start(SleepDayId sleep_day,
                             uint32_t generation,
                             StorageReadLane lane);
    bool poll();
    void reset();
    void cancel();

    const StorageBoundedFileLoadStatus &status() const { return status_; }
    ReportSignalStoreMetadata take_completed();

private:
    StorageBoundedFileLoader loader_;
    StorageBoundedFileLoadStatus status_;
    SleepDayId sleep_day_;
    ReportSignalStoreMetadata completed_;
};

}  // namespace aircannect
