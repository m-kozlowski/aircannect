#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_artifacts.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportArtifactLookupState : uint8_t {
    Idle,
    SubmitManifest,
    WaitManifest,
    Ready,
    MissingManifest,
    MissingArtifact,
    Failed,
    Cancelled,
};

struct ReportArtifactLookupStatus {
    ReportArtifactLookupState state = ReportArtifactLookupState::Idle;
    ReportArtifactKey request;
    uint32_t generation = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const;
    bool terminal() const;
};

class ReportArtifactLookupService {
public:
    ReportArtifactLookupService() = default;
    ~ReportArtifactLookupService();

    ReportArtifactLookupService(const ReportArtifactLookupService &) = delete;
    ReportArtifactLookupService &operator=(
        const ReportArtifactLookupService &) = delete;

    void begin(StorageReadPort &read_port);
    OperationAdmission start(const ReportArtifactKey &request,
                             uint32_t generation,
                             StorageReadLane lane);
    bool poll();
    void cancel();
    void reset();

    const ReportArtifactLookupStatus &status() const { return status_; }
    const ReportArtifactAvailability &availability() const {
        return availability_;
    }

private:
    bool submit_manifest();
    bool finish_manifest();
    void finish(ReportArtifactLookupState state,
                const char *error,
                bool keep_availability = false);
    void release_prepared();

    StorageReadPort *read_port_ = nullptr;
    OperationTicket ticket_;
    StoragePreparedRead prepared_;
    StorageReadLane lane_ = StorageReadLane::Report;
    ReportArtifactLookupStatus status_;
    ReportArtifactAvailability availability_;
};

}  // namespace aircannect
