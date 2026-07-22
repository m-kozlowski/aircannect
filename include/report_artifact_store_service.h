#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_artifacts.h"
#include "storage_atomic_write_port.h"

namespace aircannect {

enum class ReportArtifactStoreState : uint8_t {
    Idle,
    PublishingResult,
    PublishingOverview,
    PublishingManifest,
    Ready,
    Failed,
    Cancelled,
};

struct ReportArtifactStoreStatus {
    ReportArtifactStoreState state = ReportArtifactStoreState::Idle;
    ReportArtifactKey key;
    uint64_t bytes_written = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const;
    bool terminal() const;
    OperationOutcome outcome() const;
};

class ReportArtifactStoreService {
public:
    ReportArtifactStoreService() = default;
    ~ReportArtifactStoreService();

    ReportArtifactStoreService(const ReportArtifactStoreService &) = delete;
    ReportArtifactStoreService &operator=(
        const ReportArtifactStoreService &) = delete;

    void begin(StorageAtomicWritePort &write_port);

    OperationAdmission start(
        std::shared_ptr<const ReportArtifactBundle> bundle,
        uint32_t generation,
        StorageAtomicWriteLane lane);
    bool poll();
    void cancel();
    void reset();

    const ReportArtifactStoreStatus &status() const { return status_; }
    std::shared_ptr<const ReportArtifactBundle> published() const;

private:
    enum class Phase : uint8_t {
        Idle,
        SubmitResult,
        WaitResult,
        SubmitOverview,
        WaitOverview,
        SubmitManifest,
        WaitManifest,
        Ready,
        Failed,
        Cancelled,
    };

    bool submit_current();
    bool finish_current();
    void fail(const char *error);
    void clear_operation();

    StorageAtomicWritePort *write_port_ = nullptr;
    std::shared_ptr<const ReportArtifactBundle> bundle_;
    OperationTicket write_ticket_;
    uint32_t generation_ = 0;
    StorageAtomicWriteLane lane_ = StorageAtomicWriteLane::Maintenance;
    Phase phase_ = Phase::Idle;
    ReportArtifactStoreStatus status_;
};

}  // namespace aircannect
