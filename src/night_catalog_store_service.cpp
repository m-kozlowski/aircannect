#include "night_catalog_store_service.h"

#include <string.h>
#include <stdio.h>
#include <new>
#include <utility>
#include <vector>

#include "large_byte_buffer.h"
#include "large_allocator.h"
#include "large_object.h"
#include "night_catalog_builder.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr const char *CATALOG_STAGING_ROOT =
    "/aircannect/report/catalog-staging";

enum class StoreWriteKind : uint8_t {
    None,
    Archive,
    SaveSource,
    StageSource,
    InstallSource,
    Index,
};

struct StagedNight {
    SleepDayId sleep_day;
    uint64_t source_revision = 0;
    uint32_t source_flags = 0;
    uint64_t size = 0;
    bool installed = false;
};

using StagedNightList =
    std::vector<StagedNight, LargeAllocator<StagedNight>>;

}  // namespace

struct NightCatalogStoreRuntime {
    enum class Phase : uint8_t {
        Idle,
        CleanupStartDelete,
        CleanupWaitDelete,
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
    uint32_t delete_id = 0;
    bool cleanup_cancelled = false;
    StoragePreparedRead prepared;
    NightCatalogFileInfo file_info;
    SleepDayId loading_day;
    StorageReadLane read_lane = StorageReadLane::Report;
    char path[AC_STORAGE_PATH_MAX] = {};
    char staged_path[AC_STORAGE_PATH_MAX] = {};
    uint8_t header[NightCatalogFileCodec::HeaderBytes] = {};
    std::unique_ptr<LargeByteBuffer> body;

    std::shared_ptr<const LargeByteBuffer> encoded;
    std::shared_ptr<const LargeByteBuffer> index_encoded;
    std::shared_ptr<const NightCatalog> saving;
    std::shared_ptr<const NightCatalog> index;
    StagedNightList staged;
    size_t install_stage_index = 0;
    uint64_t install_size = 0;
    StagedNight pending_stage;
    bool pending_stage_valid = false;
    size_t next_record = 0;
    size_t history_record = 0;
    NightCatalogStoreService history_loader;
    SleepDayId history_sources_day;
    bool history_sources_requested = false;
    StoreWriteKind write_kind = StoreWriteKind::None;
    uint32_t staging_generation = 0;
    bool staging = false;
    bool staging_ready = false;
    bool installing = false;
    bool cleanup_pending = false;
    bool staging_root_clean = false;
};

NightCatalogStoreService::~NightCatalogStoreService() {
    cancel();
    if (runtime_ && runtime_->delete_id && delete_port_) {
        (void)delete_port_->cancel(runtime_->delete_id);
    }
    LargeObject::destroy(runtime_);
}

void NightCatalogStoreService::begin(StorageReadPort &read_port) {
    cancel();
    if (!runtime_) runtime_ = LargeObject::create<NightCatalogStoreRuntime>();

    read_port_ = &read_port;
    write_port_ = nullptr;
    if (!runtime_ || runtime_->delete_id == 0) delete_port_ = nullptr;
}

void NightCatalogStoreService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port) {
    begin(read_port);
    write_port_ = &write_port;
}

void NightCatalogStoreService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port,
    StorageDeletePort &delete_port) {
    begin(read_port, write_port);
    delete_port_ = &delete_port;
    if (runtime_) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
    }
}

bool NightCatalogStoreService::active() const {
    if (!runtime_) return false;
    return runtime_->phase != NightCatalogStoreRuntime::Phase::Idle &&
           runtime_->phase != NightCatalogStoreRuntime::Phase::Ready &&
           runtime_->phase != NightCatalogStoreRuntime::Phase::Error;
}

bool NightCatalogStoreService::staging() const {
    return runtime_ && runtime_->staging;
}

bool NightCatalogStoreService::installing() const {
    return runtime_ && runtime_->installing;
}

bool NightCatalogStoreService::cleanup_pending() const {
    return runtime_ && runtime_->cleanup_pending;
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
    runtime_->staged_path[0] = '\0';
    memset(runtime_->header, 0, sizeof(runtime_->header));
    runtime_->body.reset();
    runtime_->encoded.reset();
    runtime_->index_encoded.reset();
    runtime_->saving.reset();
    runtime_->index.reset();
    runtime_->install_stage_index = 0;
    runtime_->install_size = 0;
    runtime_->pending_stage = {};
    runtime_->pending_stage_valid = false;
    runtime_->next_record = 0;
    runtime_->history_record = 0;
    runtime_->history_loader.forget_media();
    runtime_->history_sources_day = {};
    runtime_->history_sources_requested = false;
    runtime_->write_kind = StoreWriteKind::None;
}

void NightCatalogStoreService::clear_staging() {
    if (!runtime_) return;

    StagedNightList empty;
    runtime_->staged.swap(empty);
    runtime_->staging = false;
    runtime_->staging_ready = false;
    runtime_->installing = false;
    runtime_->staging_generation = 0;
}

void NightCatalogStoreService::fail(const char *error) {
    if (!runtime_) return;

    const bool failed_staging = runtime_->staging;
    reset_operation();
    if (failed_staging) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
        clear_staging();
    }
    runtime_->phase = NightCatalogStoreRuntime::Phase::Error;
    status_.state = NightCatalogStoreState::Error;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "night_catalog_store_failed");
}

OperationAdmission NightCatalogStoreService::reject_staging(
    uint32_t generation,
    const char *error) {
    if (!runtime_) return OperationAdmission::Rejected;

    status_ = {};
    status_.state = NightCatalogStoreState::Error;
    status_.generation = generation;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "night_catalog_staging_rejected");
    return OperationAdmission::Rejected;
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
    if (active() || staging()) return OperationAdmission::Busy;

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
    if (!runtime_ || !read_port_ || active() || staging()) {
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

namespace {

bool staged_path(SleepDayId day,
                 char *out,
                 size_t out_size) {
    char day_text[9] = {};
    if (!day.format_yyyymmdd(day_text, sizeof(day_text))) return false;

    const int written = snprintf(out, out_size, "%s/%s.sources.bin",
                                 CATALOG_STAGING_ROOT, day_text);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

size_t find_staged_night(const StagedNightList &staged,
                         const NightCatalogRecord &record) {
    for (size_t i = 0; i < staged.size(); ++i) {
        const StagedNight &candidate = staged[i];
        if (candidate.sleep_day == record.sleep_day &&
            candidate.source_revision == record.source_revision.value() &&
            candidate.source_flags == record.source_flags &&
            candidate.size > 0) {
            return i;
        }
    }
    return staged.size();
}

size_t find_staged_day(const StagedNightList &staged, SleepDayId day) {
    for (size_t i = 0; i < staged.size(); ++i) {
        if (staged[i].sleep_day == day) return i;
    }
    return staged.size();
}

}  // namespace

OperationAdmission NightCatalogStoreService::begin_staging(
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_ || !delete_port_ ||
        generation == 0) {
        return reject_staging(generation, "night_catalog_staging_unavailable");
    }
    if (active() || staging()) return OperationAdmission::Busy;

    reset_operation();

    clear_staging();
    runtime_->staging = true;
    runtime_->staging_generation = generation;
    const bool root_clean = runtime_->staging_root_clean &&
                            !runtime_->cleanup_pending;
    runtime_->cleanup_pending = !root_clean;
    if (root_clean) {
        runtime_->staging_ready = true;
        runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
    } else {
        runtime_->phase = NightCatalogStoreRuntime::Phase::CleanupStartDelete;
    }
    status_ = {};
    status_.state = root_clean ? NightCatalogStoreState::Ready
                               : NightCatalogStoreState::Saving;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

OperationAdmission NightCatalogStoreService::request_stage(
    std::shared_ptr<const NightCatalog> single_night,
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_) {
        return OperationAdmission::Rejected;
    }
    if (active()) return OperationAdmission::Busy;
    if (!staging() || !runtime_->staging_ready || runtime_->installing) {
        return OperationAdmission::Rejected;
    }
    if (generation == 0 || generation != runtime_->staging_generation) {
        return reject_staging(generation,
                              "night_catalog_staging_generation_mismatch");
    }
    if (!single_night || single_night->size() != 1 ||
        !single_night->record(0) ||
        !single_night->record(0)->sleep_day.valid()) {
        return reject_staging(generation, "night_catalog_stage_invalid");
    }

    reset_operation();
    runtime_->saving = std::move(single_night);
    runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
    status_ = {};
    status_.state = NightCatalogStoreState::Saving;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

OperationAdmission NightCatalogStoreService::request_install(
    std::shared_ptr<const NightCatalog> compact_index,
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_) {
        return OperationAdmission::Rejected;
    }
    if (active()) return OperationAdmission::Busy;
    if (!staging() || !runtime_->staging_ready || runtime_->installing) {
        return OperationAdmission::Rejected;
    }
    if (generation == 0 || generation != runtime_->staging_generation) {
        return reject_staging(generation,
                              "night_catalog_staging_generation_mismatch");
    }
    if (!compact_index) {
        return reject_staging(generation, "night_catalog_install_invalid");
    }

    for (size_t i = 0; i < compact_index->size(); ++i) {
        const NightCatalogRecord *record = compact_index->record(i);
        if (record->sources_external && !sources_stored(*record) &&
            find_staged_night(runtime_->staged, *record) ==
                runtime_->staged.size()) {
            return reject_staging(
                generation, "night_catalog_install_sources_missing_or_stale");
        }
    }

    const auto encoded_index = NightCatalogFileCodec::encode(*compact_index);
    if (!encoded_index) {
        return reject_staging(generation,
                              "night_catalog_install_encode_failed");
    }

    reset_operation();
    runtime_->saving = compact_index;
    runtime_->index = std::move(compact_index);
    runtime_->index_encoded = encoded_index;
    runtime_->installing = true;
    runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
    status_ = {};
    status_.state = NightCatalogStoreState::Saving;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

OperationAdmission NightCatalogStoreService::request_save(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (!runtime_ || !read_port_ || !write_port_) {
        return OperationAdmission::Rejected;
    }
    if (active() || staging()) return OperationAdmission::Busy;
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
            if (runtime_->cleanup_pending && delete_port_) {
                runtime_->phase = NightCatalogStoreRuntime::Phase::CleanupStartDelete;
                status_.state = NightCatalogStoreState::Saving;
                status_.bytes = 0;
                status_.error[0] = '\0';
                return true;
            }
            return false;

        case NightCatalogStoreRuntime::Phase::CleanupStartDelete: {
            if (!delete_port_) {
                if (runtime_->staging) {
                    fail("night_catalog_staging_cleanup_unavailable");
                } else {
                    runtime_->phase = published_
                        ? NightCatalogStoreRuntime::Phase::Ready
                        : NightCatalogStoreRuntime::Phase::Idle;
                }
                return true;
            }
            if (delete_port_->active()) return false;

            const char *names[] = {"catalog-staging"};
            char error[AC_STORAGE_ERROR_MAX] = {};
            if (!delete_port_->start_selected(
                    "/aircannect/report", names, 1,
                    &runtime_->delete_id, error, sizeof(error))) {
                if (delete_port_->active() || !error[0] ||
                    strstr(error, "busy") ||
                    strstr(error, "unavailable")) {
                    return false;
                }
                if (runtime_->staging) {
                    fail(error[0] ? error : "night_catalog_staging_cleanup_failed");
                } else {
                    runtime_->phase = NightCatalogStoreRuntime::Phase::Error;
                    status_.state = NightCatalogStoreState::Error;
                    copy_cstr(status_.error, sizeof(status_.error), error);
                }
                return true;
            }

            runtime_->cleanup_cancelled = false;
            runtime_->phase = NightCatalogStoreRuntime::Phase::CleanupWaitDelete;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::CleanupWaitDelete: {
            StorageDeleteStatus deletion;
            if (!delete_port_->status(deletion, 0)) return false;
            if (deletion.id != runtime_->delete_id) {
                if (delete_port_->active()) return false;
                runtime_->delete_id = 0;
                fail("night_catalog_staging_cleanup_status_lost");
                return true;
            }
            if (deletion.state == StorageDeleteState::Deleting) return false;

            runtime_->delete_id = 0;
            if (runtime_->cleanup_cancelled) {
                runtime_->cleanup_cancelled = false;
                runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
                status_.state = NightCatalogStoreState::Ready;
                status_.bytes = 0;
                status_.error[0] = '\0';
                return true;
            }
            if (deletion.state != StorageDeleteState::Done) {
                // The delete service reports a missing fixed base as
                // not_directory. No staging child can exist in that case.
                if (deletion.state == StorageDeleteState::Error &&
                    strcmp(deletion.error, "not_directory") == 0) {
                    runtime_->cleanup_pending = false;
                    runtime_->staging_root_clean = true;
                    runtime_->staging_ready = runtime_->staging;
                    runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
                    status_.state = NightCatalogStoreState::Ready;
                    status_.error[0] = '\0';
                    return true;
                }
                if (runtime_->staging) {
                    fail(deletion.error[0]
                             ? deletion.error
                             : "night_catalog_staging_cleanup_failed");
                } else {
                    runtime_->phase = NightCatalogStoreRuntime::Phase::Error;
                    status_.state = NightCatalogStoreState::Error;
                    copy_cstr(status_.error, sizeof(status_.error),
                              deletion.error[0]
                                  ? deletion.error
                                  : "night_catalog_staging_cleanup_failed");
                }
                return true;
            }

            runtime_->cleanup_pending = false;
            runtime_->staging_root_clean = true;
            runtime_->staging_ready = runtime_->staging;
            runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
            status_.state = NightCatalogStoreState::Ready;
            status_.error[0] = '\0';
            return true;
        }

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
                runtime_->write_kind = StoreWriteKind::Archive;
                runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
                return true;
            }

            while ((!runtime_->staging || runtime_->installing) && stored_ &&
                   runtime_->history_record < stored_->size()) {
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
                runtime_->write_kind = StoreWriteKind::Archive;
                runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
                return true;
            }

            const NightCatalogRecord *record = nullptr;
            std::shared_ptr<const NightCatalog> next;

            if (runtime_->installing) {
                while (runtime_->next_record < runtime_->saving->size()) {
                    record = runtime_->saving->record(runtime_->next_record++);
                    if (!record) {
                        fail("night_catalog_install_invalid");
                        return true;
                    }
                    if (sources_stored(*record)) {
                        record = nullptr;
                        continue;
                    }

                    const size_t staged_index =
                        find_staged_night(runtime_->staged, *record);
                    if (staged_index == runtime_->staged.size()) {
                        fail("night_catalog_install_sources_missing_or_stale");
                        return true;
                    }

                    StagedNight &staged = runtime_->staged[staged_index];
                    if (staged.installed) {
                        record = nullptr;
                        continue;
                    }
                    if (!NightCatalogFileCodec::sources_path(
                            record->sleep_day, runtime_->path,
                            sizeof(runtime_->path))) {
                        fail("night_catalog_install_path_invalid");
                        return true;
                    }

                    char scratch[AC_STORAGE_PATH_MAX] = {};
                    if (!staged_path(staged.sleep_day, scratch,
                                     sizeof(scratch))) {
                        fail("night_catalog_staging_path_invalid");
                        return true;
                    }

                    runtime_->install_stage_index = staged_index;
                    runtime_->install_size = staged.size;
                    runtime_->write_kind = StoreWriteKind::InstallSource;
                    copy_cstr(runtime_->staged_path,
                              sizeof(runtime_->staged_path), scratch);
                    runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
                    return true;
                }

                runtime_->write_kind = StoreWriteKind::Index;
                copy_cstr(runtime_->path, sizeof(runtime_->path),
                          NIGHT_CATALOG_STORE_PATH);
                runtime_->encoded = runtime_->index_encoded;
                if (!runtime_->encoded) {
                    fail("night_catalog_install_encode_failed");
                    return true;
                }
                status_.bytes += runtime_->encoded->size();
                runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
                return true;
            } else if (runtime_->staging) {
                record = runtime_->saving->record(0);
                if (!record) {
                    fail("night_catalog_stage_invalid");
                    return true;
                }

                if (sources_stored(*record)) {
                    reset_operation();
                    runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
                    status_.state = NightCatalogStoreState::Ready;
                    status_.error[0] = '\0';
                    return true;
                }
                if (record->sources_external) {
                    fail("night_catalog_stage_sources_missing_or_stale");
                    return true;
                }

                if (!staged_path(record->sleep_day, runtime_->path,
                                 sizeof(runtime_->path))) {
                    fail("night_catalog_staging_path_invalid");
                    return true;
                }

                next = runtime_->saving;
                runtime_->write_kind = StoreWriteKind::StageSource;
            } else {
                while (runtime_->next_record < runtime_->saving->size()) {
                    record = runtime_->saving->record(runtime_->next_record++);

                    if (!record) {
                        fail("night_catalog_save_invalid");
                        return true;
                    }

                    if (!sources_stored(*record)) break;
                    record = nullptr;
                }

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
                    runtime_->write_kind = StoreWriteKind::SaveSource;
                } else {
                    // The index is the commit point: every sources write has
                    // completed successfully before reaching this phase.
                    runtime_->index = NightCatalogBuilder::index(*runtime_->saving);
                    next = runtime_->index;
                    runtime_->write_kind = StoreWriteKind::Index;

                    copy_cstr(runtime_->path, sizeof(runtime_->path),
                              NIGHT_CATALOG_STORE_PATH);
                }
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
            if (runtime_->write_kind == StoreWriteKind::StageSource) {
                runtime_->pending_stage.sleep_day = record->sleep_day;
                runtime_->pending_stage.source_revision =
                    record->source_revision.value();
                runtime_->pending_stage.source_flags = record->source_flags;
                runtime_->pending_stage.size = runtime_->encoded->size();
                runtime_->pending_stage.installed = false;
                runtime_->pending_stage_valid = true;
            }
            runtime_->phase = NightCatalogStoreRuntime::Phase::SubmitWrite;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::SubmitWrite: {
            if (!runtime_->saving ||
                (runtime_->write_kind != StoreWriteKind::InstallSource &&
                 !runtime_->encoded)) {
                fail("night_catalog_save_not_ready");
                return true;
            }

            StorageAtomicWriteCommand command;
            command.path = runtime_->path;
            if (runtime_->write_kind == StoreWriteKind::InstallSource) {
                command.staged_path = runtime_->staged_path;
                command.staged_size = runtime_->install_size;
            } else {
                command.bytes = runtime_->encoded;
            }
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
            if (runtime_->write_kind == StoreWriteKind::StageSource) {
                runtime_->staging_root_clean = false;
            }
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

            const uint64_t expected_bytes =
                runtime_->write_kind == StoreWriteKind::InstallSource
                    ? runtime_->install_size
                    : runtime_->encoded ? runtime_->encoded->size() : 0;
            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                expected_bytes == 0 ||
                completion.bytes_written != expected_bytes) {
                fail(completion.error[0]
                         ? completion.error
                         : "night_catalog_save_failed");
                return true;
            }

            if (runtime_->write_kind == StoreWriteKind::StageSource) {
                if (!runtime_->pending_stage_valid) {
                    fail("night_catalog_staging_ledger_invalid");
                    return true;
                }

                const size_t existing = find_staged_day(
                    runtime_->staged, runtime_->pending_stage.sleep_day);
                if (existing == runtime_->staged.size()) {
                    try {
                        runtime_->staged.push_back(runtime_->pending_stage);
                    } catch (const std::bad_alloc &) {
                        fail("night_catalog_staging_ledger_alloc_failed");
                        return true;
                    }
                } else {
                    runtime_->staged[existing] = runtime_->pending_stage;
                }

                runtime_->pending_stage_valid = false;
                reset_operation();
                runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
                status_.state = NightCatalogStoreState::Ready;
                status_.error[0] = '\0';
                return true;
            }

            if (runtime_->write_kind == StoreWriteKind::InstallSource) {
                if (runtime_->install_stage_index >= runtime_->staged.size()) {
                    fail("night_catalog_install_ledger_invalid");
                    return true;
                }

                runtime_->staged[runtime_->install_stage_index].installed = true;
                status_.bytes += runtime_->install_size;
                runtime_->encoded.reset();
                runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
                return true;
            }

            if (runtime_->write_kind == StoreWriteKind::Archive ||
                runtime_->write_kind == StoreWriteKind::SaveSource) {
                runtime_->encoded.reset();
                runtime_->write_kind = StoreWriteKind::None;
                runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
                return true;
            }

            if (runtime_->write_kind != StoreWriteKind::Index ||
                !runtime_->index) {
                fail("night_catalog_save_state_invalid");
                return true;
            }

            published_ = runtime_->index;
            stored_ = published_;
            const bool installed_staging = runtime_->installing;
            reset_operation();
            if (installed_staging) {
                runtime_->cleanup_pending = false;
                runtime_->staging_root_clean = true;
                clear_staging();
            }
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

    const uint32_t delete_id = runtime_->delete_id;
    const bool cancel_cleanup = delete_id != 0 && delete_port_;
    const bool discard_ledger = runtime_->staging;
    reset_operation();

    if (discard_ledger) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
        clear_staging();
    }

    if (cancel_cleanup) {
        (void)delete_port_->cancel(delete_id);
        runtime_->cleanup_cancelled = true;
        runtime_->phase = NightCatalogStoreRuntime::Phase::CleanupWaitDelete;
        status_.state = NightCatalogStoreState::Saving;
        status_.error[0] = '\0';
        return;
    }

    runtime_->cleanup_cancelled = false;
    runtime_->phase = published_ ? NightCatalogStoreRuntime::Phase::Ready
                                 : NightCatalogStoreRuntime::Phase::Idle;
    status_.state = published_ ? NightCatalogStoreState::Ready
                               : NightCatalogStoreState::Idle;
    status_.bytes = 0;
    status_.error[0] = '\0';
}

void NightCatalogStoreService::forget_media() {
    cancel();

    if (runtime_ && delete_port_) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
    }

    stored_.reset();
    published_.reset();
    status_ = {};
}

}  // namespace aircannect
