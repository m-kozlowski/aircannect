#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "report_artifacts.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportArtifactPayloadLoadState : uint8_t {
    Idle,
    Submitting,
    Waiting,
    Copying,
    Ready,
    Error,
    Cancelled,
};

struct ReportArtifactPayloadLoadStatus {
    ReportArtifactPayloadLoadState state =
        ReportArtifactPayloadLoadState::Idle;
    ReportArtifactDescriptor artifact;
    size_t bytes_loaded = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const;
    bool terminal() const;
};

class ReportArtifactPayloadLoader {
public:
    ReportArtifactPayloadLoader() = default;
    ~ReportArtifactPayloadLoader();

    ReportArtifactPayloadLoader(const ReportArtifactPayloadLoader &) = delete;
    ReportArtifactPayloadLoader &operator=(
        const ReportArtifactPayloadLoader &) = delete;

    void begin(StorageReadPort &read_port);
    OperationAdmission start(const ReportArtifactDescriptor &artifact,
                             uint32_t generation,
                             StorageReadLane lane);
    bool poll();
    void cancel();
    void reset();

    const ReportArtifactPayloadLoadStatus &status() const { return status_; }
    std::shared_ptr<const LargeByteBuffer> take_completed();

private:
    bool submit_chunk();
    bool finish_chunk_read();
    bool copy_chunk();
    void finish();
    void fail(const char *error);
    void release_operation();

    StorageReadPort *read_port_ = nullptr;
    std::unique_ptr<LargeByteBuffer> buffer_;
    std::shared_ptr<const LargeByteBuffer> completed_;
    OperationTicket ticket_;
    StoragePreparedRead prepared_;
    uint32_t generation_ = 0;
    uint32_t crc_state_ = 0;
    size_t offset_ = 0;
    size_t chunk_length_ = 0;
    size_t chunk_copied_ = 0;
    StorageReadLane lane_ = StorageReadLane::Maintenance;
    ReportArtifactPayloadLoadStatus status_;
};

}  // namespace aircannect
