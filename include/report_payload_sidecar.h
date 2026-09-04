#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "operation_outcome.h"
#include "report_artifact_payload.h"
#include "report_artifact_payload_loader.h"
#include "storage_atomic_write_port.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportPayloadSidecarRecordState : uint8_t {
    Ready = 1,
    NotBeneficial = 2,
};

struct ReportPayloadSidecarInfo {
    ReportPayloadSidecarRecordState state =
        ReportPayloadSidecarRecordState::Ready;
    uint32_t encoded_size = 0;
};

class ReportPayloadSidecarCodec {
public:
    static constexpr uint16_t Version = 1;
    static constexpr size_t HeaderBytes = 44;

    static bool path(const ReportArtifactPayloadDescriptor &payload,
                     char *out,
                     size_t out_size);
    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> encoded);
    static std::shared_ptr<const LargeByteBuffer> encode_not_beneficial(
        const ReportArtifactPayloadDescriptor &payload);
    static bool inspect(const uint8_t *header,
                        size_t length,
                        const ReportArtifactPayloadDescriptor &payload,
                        ReportPayloadSidecarInfo &info);
};

enum class ReportPayloadSidecarState : uint8_t {
    Idle,
    Loading,
    Saving,
    Ready,
    NotBeneficial,
    Missing,
    Error,
    Cancelled,
};

struct ReportPayloadSidecarStatus {
    ReportPayloadSidecarState state = ReportPayloadSidecarState::Idle;
    ReportArtifactPayloadDescriptor payload;
    StorageReadLane lane = StorageReadLane::Maintenance;
    bool body_loaded = false;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const {
        return state == ReportPayloadSidecarState::Loading ||
               state == ReportPayloadSidecarState::Saving;
    }
    bool terminal() const {
        return state == ReportPayloadSidecarState::Ready ||
               state == ReportPayloadSidecarState::NotBeneficial ||
               state == ReportPayloadSidecarState::Missing ||
               state == ReportPayloadSidecarState::Error ||
               state == ReportPayloadSidecarState::Cancelled;
    }
};

class ReportPayloadSidecarService {
public:
    ReportPayloadSidecarService() = default;
    ~ReportPayloadSidecarService();

    ReportPayloadSidecarService(const ReportPayloadSidecarService &) = delete;
    ReportPayloadSidecarService &operator=(
        const ReportPayloadSidecarService &) = delete;

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port);
    OperationAdmission request_load(
        const ReportArtifactPayloadDescriptor &payload,
        uint32_t generation,
        StorageReadLane lane,
        bool load_body);
    OperationAdmission request_save(
        const ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> encoded,
        uint32_t generation);
    OperationAdmission request_save_not_beneficial(
        const ReportArtifactPayloadDescriptor &payload,
        uint32_t generation);
    bool poll();
    void cancel();
    void reset();

    const ReportPayloadSidecarStatus &status() const { return status_; }
    std::shared_ptr<const LargeByteBuffer> take_completed();

private:
    struct Runtime;

    OperationAdmission start_save(
        const ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> bytes,
        uint32_t generation);
    void fail(const char *error);
    void finish(ReportPayloadSidecarState state);
    void release_operation();

    StorageReadPort *read_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    Runtime *runtime_ = nullptr;
    ReportPayloadSidecarStatus status_;
};

}  // namespace aircannect
