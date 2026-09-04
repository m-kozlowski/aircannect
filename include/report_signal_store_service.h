#pragma once

#include <memory>
#include <stdint.h>

#include "report_signal_store.h"
#include "storage_atomic_write_port.h"

namespace aircannect {

enum class ReportSignalStoreState : uint8_t {
    Idle,
    PublishingSignals,
    PublishingEvents,
    PublishingMetadata,
    Ready,
    Failed,
    Cancelled,
};

struct ReportSignalStoreStatus {
    ReportSignalStoreState state = ReportSignalStoreState::Idle;
    SleepDayId sleep_day;
    size_t signal_index = 0;
    size_t signal_count = 0;
    uint64_t bytes_written = 0;
    uint64_t metadata_modified = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const;
    bool terminal() const;
};

class ReportSignalStoreService {
public:
    ReportSignalStoreService() = default;
    ~ReportSignalStoreService();

    ReportSignalStoreService(const ReportSignalStoreService &) = delete;
    ReportSignalStoreService &operator=(
        const ReportSignalStoreService &) = delete;

    void begin(StorageAtomicWritePort &write_port);
    OperationAdmission start(
        std::shared_ptr<const ReportSignalStoreBundle> bundle,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane);
    bool poll();
    void cancel();
    void reset();

    const ReportSignalStoreStatus &status() const { return status_; }
    std::shared_ptr<const ReportSignalStoreBundle> published() const;

private:
    enum class Phase : uint8_t {
        Idle,
        SubmitSignal,
        WaitSignal,
        SubmitEvents,
        WaitEvents,
        SubmitMetadata,
        WaitMetadata,
        Ready,
        Failed,
        Cancelled,
    };

    std::shared_ptr<const LargeByteBuffer> current_bytes() const;
    bool current_path(char *path, size_t path_size) const;
    bool submit_current();
    bool finish_current();
    void fail(const char *error);
    void clear_operation();

    StorageAtomicWritePort *write_port_ = nullptr;
    std::shared_ptr<const ReportSignalStoreBundle> bundle_;
    OperationTicket write_ticket_;
    uint32_t operation_generation_ = 0;
    StorageAtomicWriteLane lane_ = StorageAtomicWriteLane::Maintenance;
    Phase phase_ = Phase::Idle;
    ReportSignalStoreStatus status_;
};

}  // namespace aircannect
