#include "report_payload_sidecar.h"

#include <new>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "little_endian.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t SIDECAR_MAGIC = 0x31444341u;  // "ACD1"
constexpr char SIDECAR_ROOT[] = "/aircannect/report/v8/http";

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

bool encode_header(uint8_t *out,
                   const ReportArtifactPayloadDescriptor &payload,
                   ReportPayloadSidecarRecordState state,
                   uint32_t encoded_size) {
    if (!out || !payload.valid() ||
        (state == ReportPayloadSidecarRecordState::Ready &&
         (encoded_size == 0 || encoded_size >= payload.size)) ||
        (state == ReportPayloadSidecarRecordState::NotBeneficial &&
         encoded_size != 0)) {
        return false;
    }

    memset(out, 0, ReportPayloadSidecarCodec::HeaderBytes);
    put_le32(out, SIDECAR_MAGIC);
    put_le16(out + 4, ReportPayloadSidecarCodec::Version);
    put_le16(out + 6, ReportPayloadSidecarCodec::HeaderBytes);
    out[8] = static_cast<uint8_t>(state);
    out[9] = static_cast<uint8_t>(payload.kind);
    put_le64(out + 12, payload.artifact.size);
    put_le32(out + 20, payload.artifact.crc32);
    put_le32(out + 24, payload.artifact.prefix_crc32);
    put_le32(out + 28, payload.offset);
    put_le32(out + 32, payload.size);
    put_le32(out + 36, payload.crc32);
    put_le32(out + 40, encoded_size);
    return true;
}

}  // namespace

bool ReportPayloadSidecarCodec::path(
    const ReportArtifactPayloadDescriptor &payload,
    char *out,
    size_t out_size) {
    if (!payload.valid() || !out || out_size == 0) return false;

    char artifact_path[AC_STORAGE_PATH_MAX] = {};
    if (!payload.artifact.path(artifact_path, sizeof(artifact_path))) {
        return false;
    }

    char sleep_day[9] = {};
    if (!payload.artifact.key.sleep_day.format_yyyymmdd(
            sleep_day, sizeof(sleep_day))) {
        return false;
    }

    const int written = snprintf(
        out,
        out_size,
        "%s/%s/%s.http1-%u-%08lx.deflate",
        SIDECAR_ROOT,
        sleep_day,
        storage_basename_from_path(artifact_path),
        static_cast<unsigned>(payload.kind),
        static_cast<unsigned long>(payload.offset));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

std::shared_ptr<const LargeByteBuffer> ReportPayloadSidecarCodec::encode(
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> encoded) {
    if (!encoded || encoded->size() == 0 ||
        encoded->size() >= payload.size || encoded->size() > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output = LargeByteBuffer::allocate(
        HeaderBytes + encoded->size());
    if (!output ||
        !encode_header(output->data(),
                       payload,
                       ReportPayloadSidecarRecordState::Ready,
                       static_cast<uint32_t>(encoded->size()))) {
        return {};
    }

    memcpy(output->data() + HeaderBytes,
           encoded->data(),
           encoded->size());
    return LargeByteBuffer::freeze(std::move(output));
}

std::shared_ptr<const LargeByteBuffer>
ReportPayloadSidecarCodec::encode_not_beneficial(
    const ReportArtifactPayloadDescriptor &payload) {
    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(HeaderBytes);
    if (!output ||
        !encode_header(output->data(),
                       payload,
                       ReportPayloadSidecarRecordState::NotBeneficial,
                       0)) {
        return {};
    }
    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportPayloadSidecarCodec::inspect(
    const uint8_t *header,
    size_t length,
    const ReportArtifactPayloadDescriptor &payload,
    ReportPayloadSidecarInfo &info) {
    info = {};
    if (!header || length != HeaderBytes || !payload.valid() ||
        get_le32(header) != SIDECAR_MAGIC ||
        get_le16(header + 4) != Version ||
        get_le16(header + 6) != HeaderBytes ||
        header[9] != static_cast<uint8_t>(payload.kind) ||
        get_le64(header + 12) != payload.artifact.size ||
        get_le32(header + 20) != payload.artifact.crc32 ||
        get_le32(header + 24) != payload.artifact.prefix_crc32 ||
        get_le32(header + 28) != payload.offset ||
        get_le32(header + 32) != payload.size ||
        get_le32(header + 36) != payload.crc32) {
        return false;
    }

    const uint8_t raw_state = header[8];
    const uint32_t encoded_size = get_le32(header + 40);
    if (raw_state ==
            static_cast<uint8_t>(ReportPayloadSidecarRecordState::Ready) &&
        encoded_size > 0 && encoded_size < payload.size) {
        info.state = ReportPayloadSidecarRecordState::Ready;
        info.encoded_size = encoded_size;
        return true;
    }
    if (raw_state == static_cast<uint8_t>(
                         ReportPayloadSidecarRecordState::NotBeneficial) &&
        encoded_size == 0) {
        info.state = ReportPayloadSidecarRecordState::NotBeneficial;
        return true;
    }
    return false;
}

struct ReportPayloadSidecarService::Runtime {
    enum class Phase : uint8_t {
        Idle,
        WaitHeader,
        WaitBody,
        SubmitWrite,
        WaitWrite,
        Terminal,
    };

    Phase phase = Phase::Idle;
    OperationTicket read_ticket;
    OperationTicket write_ticket;
    StoragePreparedRead prepared;
    ReportArtifactPayloadLoader body_loader;
    ReportPayloadSidecarInfo info;
    std::shared_ptr<const LargeByteBuffer> encoded_file;
    std::shared_ptr<const LargeByteBuffer> completed;
    uint32_t generation = 0;
    bool load_body = false;
    char path[AC_STORAGE_PATH_MAX] = {};
};

ReportPayloadSidecarService::~ReportPayloadSidecarService() {
    cancel();
    delete runtime_;
}

void ReportPayloadSidecarService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port) {
    if (!runtime_) runtime_ = new (std::nothrow) Runtime();
    read_port_ = &read_port;
    write_port_ = &write_port;
    if (runtime_) runtime_->body_loader.begin(read_port);
}

OperationAdmission ReportPayloadSidecarService::request_load(
    const ReportArtifactPayloadDescriptor &payload,
    uint32_t generation,
    StorageReadLane lane,
    bool load_body) {
    if (!runtime_ || !read_port_ || !write_port_ || status_.active()) {
        return OperationAdmission::Busy;
    }
    if (!payload.valid() || generation == 0) {
        return OperationAdmission::Rejected;
    }

    reset();
    if (!ReportPayloadSidecarCodec::path(
            payload, runtime_->path, sizeof(runtime_->path))) {
        return OperationAdmission::Rejected;
    }

    StorageReadCommand command;
    command.path = runtime_->path;
    command.length = ReportPayloadSidecarCodec::HeaderBytes;
    command.lane = lane;
    command.generation = generation;

    const OperationSubmission submission = read_port_->request_read(command);
    if (!submission.accepted()) return submission.admission;

    runtime_->read_ticket = submission.ticket;
    runtime_->generation = generation;
    runtime_->load_body = load_body;
    runtime_->phase = Runtime::Phase::WaitHeader;
    status_ = {};
    status_.state = ReportPayloadSidecarState::Loading;
    status_.payload = payload;
    status_.lane = lane;
    return OperationAdmission::Accepted;
}

OperationAdmission ReportPayloadSidecarService::start_save(
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> bytes,
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_ || status_.active()) {
        return OperationAdmission::Busy;
    }
    if (!payload.valid() || !bytes || generation == 0) {
        return OperationAdmission::Rejected;
    }

    reset();
    if (!ReportPayloadSidecarCodec::path(
            payload, runtime_->path, sizeof(runtime_->path))) {
        return OperationAdmission::Rejected;
    }
    runtime_->encoded_file = std::move(bytes);
    runtime_->generation = generation;
    runtime_->phase = Runtime::Phase::SubmitWrite;
    status_ = {};
    status_.state = ReportPayloadSidecarState::Saving;
    status_.payload = payload;
    status_.lane = StorageReadLane::Maintenance;
    return OperationAdmission::Accepted;
}

OperationAdmission ReportPayloadSidecarService::request_save(
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> encoded,
    uint32_t generation) {
    if (!runtime_ || status_.active()) return OperationAdmission::Busy;

    return start_save(
        payload,
        ReportPayloadSidecarCodec::encode(payload, std::move(encoded)),
        generation);
}

OperationAdmission
ReportPayloadSidecarService::request_save_not_beneficial(
    const ReportArtifactPayloadDescriptor &payload,
    uint32_t generation) {
    if (!runtime_ || status_.active()) return OperationAdmission::Busy;

    return start_save(
        payload,
        ReportPayloadSidecarCodec::encode_not_beneficial(payload),
        generation);
}

void ReportPayloadSidecarService::release_operation() {
    if (!runtime_) return;
    if (read_port_ && runtime_->read_ticket.valid()) {
        (void)read_port_->abandon(runtime_->read_ticket);
    }
    if (write_port_ && runtime_->write_ticket.valid()) {
        (void)write_port_->abandon(runtime_->write_ticket);
    }
    if (read_port_ && runtime_->prepared.valid()) {
        read_port_->release_prepared(runtime_->prepared);
    }

    runtime_->body_loader.reset();
    runtime_->read_ticket = {};
    runtime_->write_ticket = {};
    runtime_->prepared = {};
    runtime_->info = {};
    runtime_->encoded_file.reset();
    runtime_->completed.reset();
    runtime_->generation = 0;
    runtime_->load_body = false;
    runtime_->path[0] = '\0';
}

void ReportPayloadSidecarService::finish(
    ReportPayloadSidecarState state) {
    if (!runtime_) return;
    runtime_->phase = Runtime::Phase::Terminal;
    status_.state = state;
    status_.error[0] = '\0';
}

void ReportPayloadSidecarService::fail(const char *error) {
    if (!runtime_) return;
    release_operation();
    runtime_->phase = Runtime::Phase::Terminal;
    status_.state = ReportPayloadSidecarState::Error;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "report_sidecar_failed");
}

bool ReportPayloadSidecarService::poll() {
    if (!runtime_ || !read_port_ || !write_port_) return false;

    switch (runtime_->phase) {
        case Runtime::Phase::Idle:
        case Runtime::Phase::Terminal:
            return false;

        case Runtime::Phase::WaitHeader: {
            if (!runtime_->prepared.valid()) {
                StorageReadCompletion completion;
                if (!read_port_->take_completion(runtime_->read_ticket,
                                                  completion)) {
                    return false;
                }
                runtime_->read_ticket = {};

                if (completion.outcome.disposition !=
                        OperationDisposition::Succeeded ||
                    !completion.prepared.valid()) {
                    if (completion.prepared.valid()) {
                        read_port_->release_prepared(completion.prepared);
                    }
                    finish(ReportPayloadSidecarState::Missing);
                    return true;
                }
                runtime_->prepared = completion.prepared;
            }

            uint8_t header[ReportPayloadSidecarCodec::HeaderBytes] = {};
            const PreparedByteRead read = read_port_->read_prepared(
                runtime_->prepared, 0, header, sizeof(header));
            if (read.state == PreparedByteReadState::Retry) return false;

            read_port_->release_prepared(runtime_->prepared);
            runtime_->prepared = {};
            if (read.state != PreparedByteReadState::Data ||
                read.bytes != sizeof(header) ||
                !ReportPayloadSidecarCodec::inspect(
                    header,
                    sizeof(header),
                    status_.payload,
                    runtime_->info)) {
                finish(ReportPayloadSidecarState::Missing);
                return true;
            }

            if (runtime_->info.state ==
                ReportPayloadSidecarRecordState::NotBeneficial) {
                finish(ReportPayloadSidecarState::NotBeneficial);
                return true;
            }
            if (!runtime_->load_body) {
                finish(ReportPayloadSidecarState::Ready);
                return true;
            }

            const OperationAdmission admitted =
                runtime_->body_loader.start_encoded(
                    status_.payload,
                    runtime_->path,
                    ReportPayloadSidecarCodec::HeaderBytes,
                    runtime_->info.encoded_size,
                    runtime_->generation,
                    status_.lane);
            if (admitted == OperationAdmission::Busy) return false;
            if (admitted != OperationAdmission::Accepted) {
                fail("report_sidecar_body_rejected");
                return true;
            }

            runtime_->phase = Runtime::Phase::WaitBody;
            return true;
        }

        case Runtime::Phase::WaitBody: {
            if (runtime_->body_loader.status().active()) {
                return runtime_->body_loader.poll();
            }

            const ReportArtifactPayloadLoadStatus body_status =
                runtime_->body_loader.status();
            if (body_status.state == ReportArtifactPayloadLoadState::Ready) {
                runtime_->completed =
                    runtime_->body_loader.take_completed();
                if (!runtime_->completed) {
                    fail("report_sidecar_body_missing");
                    return true;
                }
                status_.body_loaded = true;
                finish(ReportPayloadSidecarState::Ready);
                return true;
            }
            if (body_status.terminal()) {
                fail(body_status.error[0]
                         ? body_status.error
                         : "report_sidecar_body_failed");
                return true;
            }
            return false;
        }

        case Runtime::Phase::SubmitWrite: {
            StorageAtomicWriteCommand command;
            command.path = runtime_->path;
            command.bytes = runtime_->encoded_file;
            command.lane = StorageAtomicWriteLane::Maintenance;
            command.generation = runtime_->generation;

            const OperationSubmission submission =
                write_port_->request_write(command);
            if (submission.admission == OperationAdmission::Busy) {
                return false;
            }
            if (!submission.accepted()) {
                fail("report_sidecar_write_rejected");
                return true;
            }

            runtime_->write_ticket = submission.ticket;
            runtime_->phase = Runtime::Phase::WaitWrite;
            return true;
        }

        case Runtime::Phase::WaitWrite: {
            StorageAtomicWriteCompletion completion;
            if (!write_port_->take_completion(runtime_->write_ticket,
                                               completion)) {
                return false;
            }
            runtime_->write_ticket = {};

            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                !runtime_->encoded_file ||
                completion.bytes_written != runtime_->encoded_file->size()) {
                fail(completion.error[0]
                         ? completion.error
                         : "report_sidecar_write_failed");
                return true;
            }

            finish(ReportPayloadSidecarState::Ready);
            return true;
        }
    }
    return false;
}

void ReportPayloadSidecarService::cancel() {
    if (!runtime_ || status_.state == ReportPayloadSidecarState::Idle) return;

    release_operation();
    runtime_->phase = Runtime::Phase::Terminal;
    status_.state = ReportPayloadSidecarState::Cancelled;
    status_.error[0] = '\0';
}

void ReportPayloadSidecarService::reset() {
    if (!runtime_) {
        status_ = {};
        return;
    }
    if (status_.active()) cancel();
    release_operation();
    runtime_->phase = Runtime::Phase::Idle;
    status_ = {};
}

std::shared_ptr<const LargeByteBuffer>
ReportPayloadSidecarService::take_completed() {
    if (!runtime_ || status_.state != ReportPayloadSidecarState::Ready ||
        !status_.body_loaded || !runtime_->completed) {
        return {};
    }
    return std::move(runtime_->completed);
}

}  // namespace aircannect
