#include "night_catalog_store_service.h"

#include <new>
#include <string.h>
#include <utility>

#include "large_byte_buffer.h"
#include "string_util.h"

namespace aircannect {

struct NightCatalogStoreRuntime {
    enum class Phase : uint8_t {
        Idle,
        WaitHeader,
        SubmitBody,
        WaitBody,
        SubmitWrite,
        WaitWrite,
        Ready,
        Error,
    };

    Phase phase = Phase::Idle;
    OperationTicket read_ticket;
    OperationTicket write_ticket;
    StoragePreparedRead prepared;
    NightCatalogFileInfo file_info;
    uint8_t header[NightCatalogFileCodec::HeaderBytes] = {};
    std::unique_ptr<LargeByteBuffer> body;
    std::shared_ptr<const LargeByteBuffer> encoded;
    std::shared_ptr<const NightCatalog> saving;
};

NightCatalogStoreService::~NightCatalogStoreService() {
    cancel();
    delete runtime_;
}

void NightCatalogStoreService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port) {
    if (!runtime_) runtime_ = new (std::nothrow) NightCatalogStoreRuntime();
    read_port_ = &read_port;
    write_port_ = &write_port;
}

bool NightCatalogStoreService::active() const {
    if (!runtime_) return false;
    return runtime_->phase != NightCatalogStoreRuntime::Phase::Idle &&
           runtime_->phase != NightCatalogStoreRuntime::Phase::Ready &&
           runtime_->phase != NightCatalogStoreRuntime::Phase::Error;
}

void NightCatalogStoreService::reset_operation() {
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

    runtime_->read_ticket = {};
    runtime_->write_ticket = {};
    runtime_->prepared = {};
    runtime_->file_info = {};
    memset(runtime_->header, 0, sizeof(runtime_->header));
    runtime_->body.reset();
    runtime_->encoded.reset();
    runtime_->saving.reset();
}

void NightCatalogStoreService::fail(const char *error) {
    if (!runtime_) return;

    reset_operation();
    runtime_->phase = NightCatalogStoreRuntime::Phase::Error;
    status_.state = NightCatalogStoreState::Error;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "night_catalog_store_failed");
}

OperationAdmission NightCatalogStoreService::request_load(
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (generation == 0) return OperationAdmission::Rejected;

    reset_operation();

    StorageReadCommand command;
    command.path = NIGHT_CATALOG_STORE_PATH;
    command.length = NightCatalogFileCodec::HeaderBytes;
    command.lane = StorageReadLane::Report;
    command.generation = generation;

    const OperationSubmission submission = read_port_->request_read(command);
    if (!submission.accepted()) return submission.admission;

    runtime_->read_ticket = submission.ticket;
    runtime_->phase = NightCatalogStoreRuntime::Phase::WaitHeader;
    status_ = {};
    status_.state = NightCatalogStoreState::Loading;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

OperationAdmission NightCatalogStoreService::request_save(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (!catalog || generation == 0) return OperationAdmission::Rejected;

    reset_operation();

    std::shared_ptr<const LargeByteBuffer> encoded =
        NightCatalogFileCodec::encode(*catalog);
    if (!encoded) return OperationAdmission::Rejected;

    runtime_->encoded = std::move(encoded);
    runtime_->saving = std::move(catalog);
    runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
    status_ = {};
    status_.state = NightCatalogStoreState::Saving;
    status_.generation = generation;
    status_.bytes = runtime_->encoded->size();
    return OperationAdmission::Accepted;
}

namespace {

PreparedByteRead read_exact(StorageReadPort &read_port,
                            StoragePreparedRead prepared,
                            uint8_t *out,
                            size_t expected) {
    if (!prepared.valid() || prepared.length != expected ||
        (expected > 0 && !out)) {
        return {};
    }
    if (expected == 0) return {};
    return read_port.read_prepared(prepared, 0, out, expected);
}

}  // namespace

bool NightCatalogStoreService::poll() {
    if (!runtime_ || !read_port_ || !write_port_) return false;

    switch (runtime_->phase) {
        case NightCatalogStoreRuntime::Phase::Idle:
        case NightCatalogStoreRuntime::Phase::Ready:
        case NightCatalogStoreRuntime::Phase::Error:
            return false;

        case NightCatalogStoreRuntime::Phase::WaitHeader: {
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
                    fail("night_catalog_load_header_invalid");
                    return true;
                }
                runtime_->prepared = completion.prepared;
            }

            const PreparedByteRead read = read_exact(
                *read_port_, runtime_->prepared, runtime_->header,
                sizeof(runtime_->header));
            if (read.state == PreparedByteReadState::Retry) return false;

            read_port_->release_prepared(runtime_->prepared);
            runtime_->prepared = {};
            if (read.state != PreparedByteReadState::Data ||
                read.bytes != sizeof(runtime_->header) ||
                !NightCatalogFileCodec::inspect(runtime_->header,
                                                sizeof(runtime_->header),
                                                runtime_->file_info)) {
                fail("night_catalog_load_header_invalid");
                return true;
            }

            runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitBody;
            status_.bytes = runtime_->file_info.total_bytes;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::SubmitBody: {
            if (runtime_->file_info.body_bytes == 0) {
                std::shared_ptr<const NightCatalog> catalog =
                    NightCatalogFileCodec::decode(
                        runtime_->header,
                        sizeof(runtime_->header),
                        nullptr,
                        0);
                if (!catalog) {
                    fail("night_catalog_load_decode_failed");
                    return true;
                }

                published_ = std::move(catalog);
                reset_operation();
                runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
                status_.state = NightCatalogStoreState::Ready;
                status_.error[0] = '\0';
                return true;
            }

            StorageReadCommand command;
            command.path = NIGHT_CATALOG_STORE_PATH;
            command.offset = NightCatalogFileCodec::HeaderBytes;
            command.length = runtime_->file_info.body_bytes;
            command.lane = StorageReadLane::Report;
            command.generation = status_.generation;

            const OperationSubmission submission =
                read_port_->request_read(command);
            if (submission.admission == OperationAdmission::Busy) return false;
            if (!submission.accepted()) {
                fail("night_catalog_load_body_rejected");
                return true;
            }

            runtime_->read_ticket = submission.ticket;
            runtime_->phase = NightCatalogStoreRuntime::Phase::WaitBody;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::WaitBody: {
            if (!runtime_->prepared.valid()) {
                StorageReadCompletion completion;
                if (!read_port_->take_completion(runtime_->read_ticket,
                                                  completion)) {
                    return false;
                }
                runtime_->read_ticket = {};

                if (completion.outcome.disposition !=
                        OperationDisposition::Succeeded ||
                    !completion.prepared.valid() ||
                    completion.prepared.length !=
                        runtime_->file_info.body_bytes) {
                    if (completion.prepared.valid()) {
                        read_port_->release_prepared(completion.prepared);
                    }
                    fail("night_catalog_load_body_failed");
                    return true;
                }
                runtime_->prepared = completion.prepared;
            }

            if (!runtime_->body) {
                runtime_->body = LargeByteBuffer::allocate(
                    runtime_->file_info.body_bytes);
            }
            if (!runtime_->body) {
                fail("night_catalog_load_body_short");
                return true;
            }

            const PreparedByteRead read = read_exact(
                *read_port_, runtime_->prepared, runtime_->body->data(),
                runtime_->body->size());
            if (read.state == PreparedByteReadState::Retry) return false;

            read_port_->release_prepared(runtime_->prepared);
            runtime_->prepared = {};
            if (read.state != PreparedByteReadState::Data ||
                read.bytes != runtime_->body->size()) {
                fail("night_catalog_load_body_short");
                return true;
            }

            std::shared_ptr<const NightCatalog> catalog =
                NightCatalogFileCodec::decode(runtime_->header,
                                              sizeof(runtime_->header),
                                              runtime_->body->data(),
                                              runtime_->body->size());
            if (!catalog) {
                fail("night_catalog_load_decode_failed");
                return true;
            }

            published_ = std::move(catalog);
            reset_operation();
            runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
            status_.state = NightCatalogStoreState::Ready;
            status_.error[0] = '\0';
            return true;
        }

        case NightCatalogStoreRuntime::Phase::SubmitWrite: {
            if (!runtime_->encoded || !runtime_->saving) {
                fail("night_catalog_save_not_ready");
                return true;
            }

            StorageAtomicWriteCommand command;
            command.path = NIGHT_CATALOG_STORE_PATH;
            command.bytes = runtime_->encoded;
            command.lane = StorageAtomicWriteLane::Maintenance;
            command.generation = status_.generation;

            const OperationSubmission submission =
                write_port_->request_write(command);
            if (submission.admission == OperationAdmission::Busy) return false;
            if (!submission.accepted()) {
                fail("night_catalog_save_rejected");
                return true;
            }

            runtime_->write_ticket = submission.ticket;
            runtime_->phase = NightCatalogStoreRuntime::Phase::WaitWrite;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::WaitWrite: {
            StorageAtomicWriteCompletion completion;
            if (!write_port_->take_completion(runtime_->write_ticket,
                                               completion)) {
                return false;
            }
            runtime_->write_ticket = {};

            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                !runtime_->encoded ||
                completion.bytes_written != runtime_->encoded->size()) {
                fail(completion.error[0]
                         ? completion.error
                         : "night_catalog_save_failed");
                return true;
            }

            published_ = runtime_->saving;
            reset_operation();
            runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
            status_.state = NightCatalogStoreState::Ready;
            status_.error[0] = '\0';
            return true;
        }
    }
    return false;
}

void NightCatalogStoreService::cancel() {
    if (!runtime_) return;

    reset_operation();
    runtime_->phase = published_ ? NightCatalogStoreRuntime::Phase::Ready
                                 : NightCatalogStoreRuntime::Phase::Idle;
    status_.state = published_ ? NightCatalogStoreState::Ready
                               : NightCatalogStoreState::Idle;
    status_.bytes = 0;
    status_.error[0] = '\0';
}

}  // namespace aircannect
