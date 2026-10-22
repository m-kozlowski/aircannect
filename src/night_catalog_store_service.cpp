#include "night_catalog_store_service.h"

#include <string.h>
#include <stdio.h>
#include <utility>

#include "large_byte_buffer.h"
#include "large_object.h"
#include "night_catalog_builder.h"
#include "string_util.h"

namespace aircannect {

struct NightCatalogStoreRuntime {
    enum class Phase : uint8_t {
        Idle,
        WaitHeader,
        SubmitBody,
        WaitBody,
        PrepareWrite,
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
    SleepDayId loading_day;
    StorageReadLane read_lane = StorageReadLane::Report;
    char path[AC_STORAGE_PATH_MAX] = {};
    uint8_t header[NightCatalogFileCodec::HeaderBytes] = {};
    std::unique_ptr<LargeByteBuffer> body;

    std::shared_ptr<const LargeByteBuffer> encoded;
    std::shared_ptr<const NightCatalog> saving;
    std::shared_ptr<const NightCatalog> index;
    size_t next_record = 0;
    size_t history_record = 0;
    NightCatalogStoreService history_loader;
    SleepDayId history_sources_day;
    bool history_sources_requested = false;
    bool writing_index = false;
};

NightCatalogStoreService::~NightCatalogStoreService() {
    cancel();
    LargeObject::destroy(runtime_);
}

void NightCatalogStoreService::begin(StorageReadPort &read_port) {
    cancel();
    if (!runtime_) runtime_ = LargeObject::create<NightCatalogStoreRuntime>();

    read_port_ = &read_port;
    write_port_ = nullptr;
}

void NightCatalogStoreService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port) {
    begin(read_port);
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
    runtime_->loading_day = {};
    runtime_->read_lane = StorageReadLane::Report;
    runtime_->path[0] = '\0';
    memset(runtime_->header, 0, sizeof(runtime_->header));
    runtime_->body.reset();
    runtime_->encoded.reset();
    runtime_->saving.reset();
    runtime_->index.reset();
    runtime_->next_record = 0;
    runtime_->history_record = 0;
    runtime_->history_loader.forget_media();
    runtime_->history_sources_day = {};
    runtime_->history_sources_requested = false;
    runtime_->writing_index = false;
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

OperationAdmission NightCatalogStoreService::reject_save(
    uint32_t generation,
    const char *error) {
    reset_operation();
    runtime_->phase = NightCatalogStoreRuntime::Phase::Error;
    status_ = {};
    status_.state = NightCatalogStoreState::Error;
    status_.generation = generation;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "night_catalog_save_rejected");
    return OperationAdmission::Rejected;
}

OperationAdmission NightCatalogStoreService::request_load(
    uint32_t generation) {
    return start_load(NIGHT_CATALOG_STORE_PATH, {}, generation,
                      StorageReadLane::Report);
}

OperationAdmission NightCatalogStoreService::request_load_night(
    SleepDayId sleep_day,
    uint32_t generation,
    StorageReadLane lane) {
    if (active()) return OperationAdmission::Busy;

    char path[AC_STORAGE_PATH_MAX] = {};

    if (!sleep_day.valid() ||
        !NightCatalogFileCodec::sources_path(sleep_day, path, sizeof(path))) {
        return OperationAdmission::Rejected;
    }

    return start_load(path, sleep_day, generation, lane);
}

OperationAdmission NightCatalogStoreService::start_load(
    const char *path,
    SleepDayId sleep_day,
    uint32_t generation,
    StorageReadLane lane) {
    if (!runtime_ || !read_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (generation == 0) return OperationAdmission::Rejected;

    reset_operation();

    StorageReadCommand command;
    command.path = path;
    command.length = NightCatalogFileCodec::HeaderBytes;
    command.lane = lane;
    command.generation = generation;

    const OperationSubmission submission = read_port_->request_read(command);
    if (!submission.accepted()) return submission.admission;

    copy_cstr(runtime_->path, sizeof(runtime_->path), path);
    runtime_->loading_day = sleep_day;
    runtime_->read_lane = lane;
    runtime_->read_ticket = submission.ticket;
    runtime_->phase = NightCatalogStoreRuntime::Phase::WaitHeader;
    status_ = {};
    status_.state = NightCatalogStoreState::Loading;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

void NightCatalogStoreService::finish_load(
    std::shared_ptr<const NightCatalog> catalog) {
    if (!catalog) {
        fail("night_catalog_load_decode_failed");
        return;
    }

    if (runtime_->loading_day.valid()) {
        const NightCatalogRecord *record =
            catalog->find(runtime_->loading_day);

        if (catalog->size() != 1 || !record) {
            fail("night_catalog_load_night_mismatch");
            return;
        }

        if (record->sources_external) {
            fail("night_catalog_load_night_sources_external");
            return;
        }
    } else {
        stored_ = catalog;
    }

    published_ = std::move(catalog);
    reset_operation();
    runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
    status_.state = NightCatalogStoreState::Ready;
    status_.error[0] = '\0';
}

bool NightCatalogStoreService::sources_stored(
    const NightCatalogRecord &record) const {
    const NightCatalogRecord *previous =
        stored_ ? stored_->find(record.sleep_day) : nullptr;

    // A legacy full snapshot does not prove that sources.bin exists yet.
    return previous && previous->sources_external &&
           previous->source_revision == record.source_revision &&
           previous->source_flags == record.source_flags;
}

OperationAdmission NightCatalogStoreService::request_save(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_) {
        return OperationAdmission::Rejected;
    }
    if (active()) return OperationAdmission::Busy;
    if (!catalog || generation == 0) {
        return reject_save(generation, "night_catalog_save_invalid");
    }

    reset_operation();

    // Reject dangling references before submitting any persistent writes.
    for (size_t i = 0; i < catalog->size(); ++i) {
        const NightCatalogRecord *record = catalog->record(i);
        if (!record) {
            return reject_save(generation, "night_catalog_save_invalid");
        }

        if (record->sources_external && !sources_stored(*record)) {
            return reject_save(generation,
                               "night_catalog_save_sources_missing_or_stale");
        }
    }

    runtime_->saving = std::move(catalog);
    runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
    status_ = {};
    status_.state = NightCatalogStoreState::Saving;
    status_.generation = generation;
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
    if (!runtime_ || !read_port_) return false;

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
                    fail(completion.error[0]
                             ? completion.error
                             : "night_catalog_load_header_invalid");
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

                finish_load(std::move(catalog));
                return true;
            }

            StorageReadCommand command;
            command.path = runtime_->path;
            command.offset = NightCatalogFileCodec::HeaderBytes;
            command.length = runtime_->file_info.body_bytes;
            command.lane = runtime_->read_lane;
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
                    fail(completion.error[0]
                             ? completion.error
                             : "night_catalog_load_body_failed");
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

            finish_load(std::move(catalog));
            return true;
        }

        case NightCatalogStoreRuntime::Phase::PrepareWrite: {
            if (!runtime_->saving) {
                fail("night_catalog_save_not_ready");
                return true;
            }

            // Archive the old interpretation before any sources or index
            // replacement. Revision-specific names make restart idempotent.
            if (runtime_->history_sources_day.valid()) {
                auto &loader = runtime_->history_loader;
                if (!runtime_->history_sources_requested) {
                    loader.begin(*read_port_);
                    const auto admitted = loader.request_load_night(
                        runtime_->history_sources_day, status_.generation);
                    if (admitted == OperationAdmission::Busy) return false;
                    if (admitted != OperationAdmission::Accepted) {
                        fail("night_catalog_history_sources_read_rejected");
                        return true;
                    }
                    runtime_->history_sources_requested = true;
                    return true;
                }

                const bool worked = loader.poll();
                if (loader.active()) return worked;
                if (loader.status().state != NightCatalogStoreState::Ready) {
                    if (strcmp(loader.status().error, "read_not_found") != 0) {
                        fail("night_catalog_history_sources_unavailable");
                        return true;
                    }
                    runtime_->history_sources_day = {};
                    runtime_->history_sources_requested = false;
                    return true;
                }

                const auto sources = loader.take_snapshot();
                runtime_->encoded = sources ? NightCatalogFileCodec::encode(*sources) : nullptr;
                if (!runtime_->encoded) {
                    fail("night_catalog_history_sources_encode_failed");
                    return true;
                }
                const auto &old = *sources->record(0);
                char day[9] = {};
                old.sleep_day.format_yyyymmdd(day, sizeof(day));
                snprintf(runtime_->path, sizeof(runtime_->path),
                         "/aircannect/report/v9/history/%s-%016llx.sources.bin", day,
                         static_cast<unsigned long long>(old.source_revision.value()));
                runtime_->history_sources_day = {};
                runtime_->history_sources_requested = false;
                status_.bytes += runtime_->encoded->size();
                runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
                return true;
            }

            while (stored_ && runtime_->history_record < stored_->size()) {
                const auto &old = *stored_->record(runtime_->history_record++);
                if (!old.unresolved_history) continue;
                if (old.sources_external) runtime_->history_sources_day = old.sleep_day;

                auto history = NightCatalogBuilder::select_night(*stored_, old.sleep_day);
                runtime_->encoded = history ? NightCatalogFileCodec::encode(*history) : nullptr;
                if (!runtime_->encoded) {
                    fail("night_catalog_history_preserve_failed");
                    return true;
                }

                char day[9] = {};
                old.sleep_day.format_yyyymmdd(day, sizeof(day));
                snprintf(runtime_->path, sizeof(runtime_->path),
                         "/aircannect/report/v9/history/%s-%016llx.bin", day,
                         static_cast<unsigned long long>(old.source_revision.value()));
                status_.bytes += runtime_->encoded->size();
                runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
                return true;
            }

            const NightCatalogRecord *record = nullptr;
            while (runtime_->next_record < runtime_->saving->size()) {
                record = runtime_->saving->record(runtime_->next_record++);

                if (!record) {
                    fail("night_catalog_save_invalid");
                    return true;
                }

                if (!sources_stored(*record)) break;
                record = nullptr;
            }

            std::shared_ptr<const NightCatalog> next;
            if (record) {
                if (record->sources_external) {
                    fail("night_catalog_save_sources_missing_or_stale");
                    return true;
                }

                if (!NightCatalogFileCodec::sources_path(
                        record->sleep_day, runtime_->path,
                        sizeof(runtime_->path))) {
                    fail("night_catalog_save_path_invalid");
                    return true;
                }

                next = NightCatalogBuilder::select_night(
                    *runtime_->saving, record->sleep_day);
            } else {
                // The index is the commit point: every sources write has
                // completed successfully before reaching this phase.
                runtime_->index = NightCatalogBuilder::index(*runtime_->saving);
                next = runtime_->index;
                runtime_->writing_index = true;

                copy_cstr(runtime_->path, sizeof(runtime_->path),
                          NIGHT_CATALOG_STORE_PATH);
            }

            if (!next) {
                fail("night_catalog_save_prepare_failed");
                return true;
            }

            runtime_->encoded = NightCatalogFileCodec::encode(*next);
            if (!runtime_->encoded) {
                fail("night_catalog_save_encode_failed");
                return true;
            }

            status_.bytes += runtime_->encoded->size();
            runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::SubmitWrite: {
            if (!runtime_->encoded || !runtime_->saving) {
                fail("night_catalog_save_not_ready");
                return true;
            }

            StorageAtomicWriteCommand command;
            command.path = runtime_->path;
            command.bytes = runtime_->encoded;
            command.lane = StorageAtomicWriteLane::Foreground;
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

            if (!runtime_->writing_index) {
                runtime_->encoded.reset();
                runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
                return true;
            }

            published_ = runtime_->index;
            stored_ = published_;
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

void NightCatalogStoreService::forget_media() {
    cancel();
    stored_.reset();
    published_.reset();
    status_ = {};
}

}  // namespace aircannect
