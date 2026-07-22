#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_artifacts.h"
#include "storage_atomic_write_port.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportArtifactStoreState : uint8_t {
    Idle,
    LoadingManifest,
    PublishingResult,
    PublishingOverview,
    PublishingRangeTile,
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
};

class ReportArtifactStoreService {
public:
    ReportArtifactStoreService() = default;
    ~ReportArtifactStoreService();

    ReportArtifactStoreService(const ReportArtifactStoreService &) = delete;
    ReportArtifactStoreService &operator=(
        const ReportArtifactStoreService &) = delete;

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port);

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
        SubmitManifestRead,
        WaitManifestRead,
        SubmitResult,
        WaitResult,
        SubmitOverview,
        WaitOverview,
        SubmitRangeTile,
        WaitRangeTile,
        SubmitManifest,
        WaitManifest,
        Ready,
        Failed,
        Cancelled,
    };

    bool submit_manifest_read();
    bool finish_manifest_read();
    bool submit_current();
    bool finish_current();
    std::shared_ptr<const LargeByteBuffer> current_bytes() const;
    bool current_path(char *path, size_t path_size) const;
    void fail(const char *error);
    void clear_operation();

    StorageReadPort *read_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    std::shared_ptr<const ReportArtifactBundle> bundle_;
    std::shared_ptr<const LargeByteBuffer> manifest_bytes_;
    OperationTicket write_ticket_;
    OperationTicket read_ticket_;
    StoragePreparedRead prepared_;
    uint32_t generation_ = 0;
    StorageAtomicWriteLane lane_ = StorageAtomicWriteLane::Maintenance;
    Phase phase_ = Phase::Idle;
    ReportArtifactStoreStatus status_;
};

}  // namespace aircannect
