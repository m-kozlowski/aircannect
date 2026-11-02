#include "night_catalog_store_service.h"

#include <algorithm>
#include <string.h>
#include <stdio.h>
#include <new>
#include <utility>
#include <vector>

#include "large_byte_buffer.h"
#include "large_allocator.h"
#include "large_object.h"
#include "night_catalog_builder.h"
#include "report_signal_store.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr const char *CATALOG_STAGING_ROOT =
    "/aircannect/report/catalog-staging";
constexpr const char *CATALOG_INDEX_STAGING_PATH =
    "/aircannect/report/catalog-staging/.index.bin";
constexpr const char *FLAT_NIGHTS_ROOT =
    "/aircannect/report/v9/nights-flat";

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
        LayoutInspect,
        LayoutScan,
        LayoutRenameRoot,
        LayoutSelectNight,
        LayoutEnsureParent,
        LayoutMoveNight,
        LayoutRemoveRoot,
        CleanupStartDelete,
        CleanupWaitDelete,
        WaitHeader,
        SubmitBody,
        WaitBody,
        PrepareWrite,
        SubmitWrite,
        WaitWrite,
        PrepareIndexChunk,
        SubmitIndexChunk,
        WaitIndexChunk,
        SubmitIndexFinish,
        WaitIndexFinish,
        Ready,
        Error,
    };

    Phase phase = Phase::Idle;
    OperationTicket read_ticket;
    OperationTicket write_ticket;
    OperationTicket range_ticket;
    OperationTicket layout_scan_ticket;
    OperationTicket layout_path_ticket;
    std::shared_ptr<const StorageScanSnapshot> layout_scan;
    size_t layout_cursor = 0;
    SleepDayId layout_day;
    SleepDayId layout_parent_day;
    uint8_t layout_parent_depth = 0;
    bool layout_ready = false;
    bool layout_backup_exists = false;
    bool layout_unknown_entries = false;
    uint8_t layout_root_index = 0;
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
    NightCatalogFileIndexReader index_reader;
    std::shared_ptr<LargeByteBuffer> index_chunk;
    size_t index_size = 0;
    size_t index_offset = 0;
    size_t index_chunk_size = 0;
    bool index_stage_complete = false;
    bool index_stage_touched = false;
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
    range_write_port_ = nullptr;
    write_port_ = nullptr;
    if (!runtime_ || runtime_->delete_id == 0) delete_port_ = nullptr;
}

void NightCatalogStoreService::begin(
    StorageReadPort &read_port,
    StorageRangeWritePort &range_write_port,
    StorageAtomicWritePort &write_port,
    StorageDeletePort *delete_port) {
    begin(read_port);
    range_write_port_ = &range_write_port;
    write_port_ = &write_port;
    delete_port_ = delete_port;
    if (runtime_ && delete_port_) {
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
    if (range_write_port_ && runtime_->range_ticket.valid()) {
        (void)range_write_port_->abandon(runtime_->range_ticket);
    }
    if (read_port_ && runtime_->prepared.valid()) {
        read_port_->release_prepared(runtime_->prepared);
    }
    if (layout_scans_ && runtime_->layout_scan_ticket.valid()) {
        (void)layout_scans_->abandon(runtime_->layout_scan_ticket);
    }
    if (layout_paths_ && runtime_->layout_path_ticket.valid()) {
        (void)layout_paths_->abandon(runtime_->layout_path_ticket);
    }

    runtime_->layout_scan_ticket = {};
    runtime_->layout_path_ticket = {};
    runtime_->layout_scan.reset();
    runtime_->layout_cursor = 0;
    runtime_->layout_day = {};
    runtime_->layout_parent_day = {};
    runtime_->layout_parent_depth = 0;
    runtime_->layout_backup_exists = false;
    runtime_->layout_unknown_entries = false;
    runtime_->layout_root_index = 0;

    runtime_->read_ticket = {};
    runtime_->write_ticket = {};
    runtime_->range_ticket = {};
    runtime_->prepared = {};
    runtime_->file_info = {};
    runtime_->loading_day = {};
    runtime_->read_lane = StorageReadLane::Report;
    runtime_->path[0] = '\0';
    runtime_->staged_path[0] = '\0';
    memset(runtime_->header, 0, sizeof(runtime_->header));
    runtime_->body.reset();
    runtime_->encoded.reset();
    runtime_->index_reader.reset();
    runtime_->index_chunk.reset();
    runtime_->index_size = 0;
    runtime_->index_offset = 0;
    runtime_->index_chunk_size = 0;
    runtime_->index_stage_complete = false;
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
    const bool cleanup_index_stage = runtime_->index_stage_touched;
    const bool close_index_handle = cleanup_index_stage &&
                                    !runtime_->index_stage_complete;
    reset_operation();
    if (close_index_handle && range_write_port_) {
        range_write_port_->release_handles();
    }
    if (failed_staging || (cleanup_index_stage && delete_port_)) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
    }
    if (failed_staging) {
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
    if (!runtime_ || !read_port_ || active() || staging() || !layout_ready()) {
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

bool NightCatalogStoreService::prepare_index_stream(
    std::shared_ptr<const NightCatalog> compact_index) {
    if (!runtime_ || !range_write_port_ || !write_port_ || !compact_index ||
        !runtime_->index_reader.start(compact_index)) {
        return false;
    }

    const size_t size = runtime_->index_reader.size();
    if (size == 0 || size > UINT32_MAX) {
        runtime_->index_reader.reset();
        return false;
    }

    runtime_->index = std::move(compact_index);
    runtime_->index_size = size;
    runtime_->index_offset = 0;
    runtime_->index_chunk_size = 0;
    runtime_->index_chunk.reset();
    runtime_->index_stage_complete = false;
    return true;
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
    if (!layout_ready()) return OperationAdmission::Busy;
    if (!runtime_ || !read_port_ || !range_write_port_ || !write_port_ ||
        !delete_port_ || generation == 0) {
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
    if (!runtime_ || !read_port_ || !range_write_port_ || !write_port_ ||
        !delete_port_) {
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
    if (!runtime_ || !read_port_ || !range_write_port_ || !write_port_ ||
        !delete_port_) {
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

    reset_operation();
    if (!prepare_index_stream(compact_index)) {
        reset_operation();
        return reject_staging(generation,
                              "night_catalog_install_encode_failed");
    }

    runtime_->saving = compact_index;
    runtime_->installing = true;
    runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareIndexChunk;
    status_ = {};
    status_.state = NightCatalogStoreState::Saving;
    status_.generation = generation;
    status_.bytes = runtime_->index_size;
    return OperationAdmission::Accepted;
}

OperationAdmission NightCatalogStoreService::request_save(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (!layout_ready()) return OperationAdmission::Busy;
    if (!runtime_ || !read_port_ || !range_write_port_ || !write_port_) {
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

void NightCatalogStoreService::configure_layout(
    StorageScanPort &scans, StoragePathPort &paths) {
    layout_scans_ = &scans;
    layout_paths_ = &paths;
    if (runtime_) runtime_->layout_ready = false;
}

bool NightCatalogStoreService::layout_ready() const {
    return !layout_paths_ || (runtime_ && runtime_->layout_ready);
}

OperationAdmission NightCatalogStoreService::prepare_layout(uint32_t generation) {
    if (!runtime_ || !layout_paths_ || !layout_scans_ || !generation) {
        return OperationAdmission::Rejected;
    }
    if (active() || staging()) return OperationAdmission::Busy;

    reset_operation();
    runtime_->layout_ready = false;
    runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutInspect;
    status_ = {};
    status_.state = NightCatalogStoreState::Loading;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

bool NightCatalogStoreService::poll_layout_path(
    StoragePathOperation operation, const char *source, const char *destination,
    StoragePathCompletion &out) {
    if (!runtime_->layout_path_ticket.valid()) {
        StoragePathCommand command;
        command.operation = operation;
        command.source = source;
        if (destination) command.destination = destination;
        command.generation = status_.generation;
        const auto submitted = layout_paths_->request(command);
        if (submitted.admission == OperationAdmission::Busy) return false;
        if (!submitted.accepted()) {
            fail("report_layout_path_rejected");
            return false;
        }
        runtime_->layout_path_ticket = submitted.ticket;
        return false;
    }

    if (!layout_paths_->take_completion(runtime_->layout_path_ticket, out)) return false;
    runtime_->layout_path_ticket = {};
    if (out.outcome.disposition != OperationDisposition::Succeeded) {
        fail(out.error[0] ? out.error : "report_layout_path_failed");
        return false;
    }
    return true;
}

void NightCatalogStoreService::finish_layout() {
    reset_operation();
    runtime_->layout_ready = true;
    runtime_->phase = NightCatalogStoreRuntime::Phase::Ready;
    status_.state = NightCatalogStoreState::Ready;
    status_.error[0] = '\0';
}

bool NightCatalogStoreService::poll(bool allow_layout_mutation) {
    if (!runtime_ || !read_port_) return false;

    if (!layout_ready() && write_port_ &&
        !write_port_->recovery_complete()) return false;

    const bool layout_mutation =
        runtime_->phase == NightCatalogStoreRuntime::Phase::LayoutRenameRoot ||
        runtime_->phase == NightCatalogStoreRuntime::Phase::LayoutEnsureParent ||
        runtime_->phase == NightCatalogStoreRuntime::Phase::LayoutMoveNight ||
        runtime_->phase == NightCatalogStoreRuntime::Phase::LayoutRemoveRoot;

    if (layout_mutation && !allow_layout_mutation) return false;

    switch (runtime_->phase) {
        case NightCatalogStoreRuntime::Phase::LayoutInspect: {
            StoragePathCompletion completion;
            if (!poll_layout_path(StoragePathOperation::Stat, FLAT_NIGHTS_ROOT,
                                  nullptr, completion)) return false;
            if (completion.exists && !completion.directory) {
                fail("report_layout_backup_not_directory");
                return true;
            }
            runtime_->layout_backup_exists = completion.exists;
            runtime_->layout_root_index = completion.exists ? 1 : 0;
            runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutScan;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::LayoutScan: {
            if (!runtime_->layout_scan_ticket.valid()) {
                const StorageScanRoot roots[] = {
                    {REPORT_SIGNAL_STORE_ROOT, false}, {FLAT_NIGHTS_ROOT, false}};
                StorageScanCommand command;
                command.roots = roots;
                command.root_count = runtime_->layout_backup_exists ? 2 : 1;
                command.include_directories = true;
                command.generation = status_.generation;
                const auto submitted = layout_scans_->request_scan(command);
                if (submitted.admission == OperationAdmission::Busy) return false;
                if (!submitted.accepted()) {
                    fail("report_layout_scan_rejected");
                    return true;
                }
                runtime_->layout_scan_ticket = submitted.ticket;
                return true;
            }

            StorageScanCompletion completion;
            if (!layout_scans_->take_completion(runtime_->layout_scan_ticket, completion)) return false;
            runtime_->layout_scan_ticket = {};
            if (completion.outcome.disposition != OperationDisposition::Succeeded ||
                !completion.snapshot) {
                fail(completion.error[0] ? completion.error : "report_layout_scan_failed");
                return true;
            }

            runtime_->layout_scan = std::move(completion.snapshot);
            bool flat = false;
            bool years = false;
            for (size_t i = 0; i < runtime_->layout_scan->size(); ++i) {
                StorageScanEntryView entry;
                runtime_->layout_scan->entry(i, entry);
                if (entry.root_index || strcmp(entry.path, REPORT_SIGNAL_STORE_ROOT) == 0) continue;
                const char *name = storage_basename_from_path(entry.path);
                SleepDayId day;
                flat |= entry.directory && SleepDayId::from_yyyymmdd(name, day);
                years |= entry.directory && strlen(name) == 4 && strspn(name, "0123456789") == 4;
            }
            if (flat && (years || runtime_->layout_backup_exists)) {
                fail("report_layout_mixed_roots");
            } else if (runtime_->layout_backup_exists) {
                runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutSelectNight;
            } else if (flat) {
                runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutRenameRoot;
            } else {
                finish_layout();
            }
            return true;
        }

        case NightCatalogStoreRuntime::Phase::LayoutRenameRoot: {
            StoragePathCompletion completion;
            if (!poll_layout_path(StoragePathOperation::Move, REPORT_SIGNAL_STORE_ROOT,
                                  FLAT_NIGHTS_ROOT, completion)) return false;
            runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutSelectNight;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::LayoutSelectNight: {
            if (runtime_->layout_cursor == runtime_->layout_scan->size()) {
                if (runtime_->layout_unknown_entries) finish_layout();
                else runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutRemoveRoot;
                return true;
            }

            StorageScanEntryView entry;
            runtime_->layout_scan->entry(runtime_->layout_cursor++, entry);
            if (entry.root_index != runtime_->layout_root_index ||
                strcmp(entry.path, REPORT_SIGNAL_STORE_ROOT) == 0 ||
                strcmp(entry.path, FLAT_NIGHTS_ROOT) == 0) return true;

            if (!entry.directory || !SleepDayId::from_yyyymmdd(
                    storage_basename_from_path(entry.path), runtime_->layout_day)) {
                runtime_->layout_unknown_entries = true;
                return true;
            }
            char previous[9] = {};
            char current[9] = {};
            runtime_->layout_parent_day.format_yyyymmdd(previous, sizeof(previous));
            runtime_->layout_day.format_yyyymmdd(current, sizeof(current));
            runtime_->layout_parent_depth = !previous[0] ? 0 :
                (strncmp(previous, current, 4) == 0 ? 2 : 1);
            runtime_->phase = previous[0] && strncmp(previous, current, 6) == 0
                ? NightCatalogStoreRuntime::Phase::LayoutMoveNight
                : NightCatalogStoreRuntime::Phase::LayoutEnsureParent;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::LayoutEnsureParent:
        case NightCatalogStoreRuntime::Phase::LayoutMoveNight: {
            char destination[AC_STORAGE_PATH_MAX];
            if (!report_signal_store_night_directory(runtime_->layout_day,
                                                     destination, sizeof(destination))) {
                fail("report_layout_path_invalid");
                return true;
            }

            StoragePathCompletion completion;
            if (runtime_->phase == NightCatalogStoreRuntime::Phase::LayoutEnsureParent) {
                const size_t suffix[] = {0, 5, 8};
                destination[strlen(REPORT_SIGNAL_STORE_ROOT) +
                    suffix[runtime_->layout_parent_depth]] = '\0';
                if (!poll_layout_path(StoragePathOperation::EnsureDirectory, destination,
                                      nullptr, completion)) return false;
                if (++runtime_->layout_parent_depth == 3) {
                    runtime_->layout_parent_day = runtime_->layout_day;
                    runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutMoveNight;
                }
            } else {
                char day[9];
                char source[AC_STORAGE_PATH_MAX];
                runtime_->layout_day.format_yyyymmdd(day, sizeof(day));
                storage_append_child_path(FLAT_NIGHTS_ROOT, day, source, sizeof(source));
                if (!poll_layout_path(StoragePathOperation::Move, source,
                                      destination, completion)) return false;
                runtime_->phase = NightCatalogStoreRuntime::Phase::LayoutSelectNight;
            }
            return true;
        }

        case NightCatalogStoreRuntime::Phase::LayoutRemoveRoot: {
            StoragePathCompletion completion;
            if (!poll_layout_path(StoragePathOperation::RemoveEmptyDirectory,
                                  FLAT_NIGHTS_ROOT, nullptr, completion)) return false;
            finish_layout();
            return true;
        }

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
                    runtime_->cleanup_pending = false;
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
                    runtime_->index_stage_touched = false;
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
            runtime_->index_stage_touched = false;
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

                if (!runtime_->index_stage_complete) {
                    fail("night_catalog_install_index_stage_incomplete");
                    return true;
                }

                runtime_->write_kind = StoreWriteKind::Index;
                copy_cstr(runtime_->path, sizeof(runtime_->path),
                          NIGHT_CATALOG_STORE_PATH);
                copy_cstr(runtime_->staged_path,
                          sizeof(runtime_->staged_path),
                          CATALOG_INDEX_STAGING_PATH);
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
                    // The streamed index remains private until every sources
                    // file is saved, then becomes the commit point.
                    if (!runtime_->index_stage_complete) {
                        runtime_->index =
                            NightCatalogBuilder::index(*runtime_->saving);
                        if (!runtime_->index ||
                            !prepare_index_stream(runtime_->index)) {
                            fail("night_catalog_save_encode_failed");
                            return true;
                        }

                        status_.bytes += runtime_->index_size;
                        runtime_->phase =
                            NightCatalogStoreRuntime::Phase::PrepareIndexChunk;
                        return true;
                    }

                    runtime_->write_kind = StoreWriteKind::Index;
                    copy_cstr(runtime_->path, sizeof(runtime_->path),
                              NIGHT_CATALOG_STORE_PATH);
                    copy_cstr(runtime_->staged_path,
                              sizeof(runtime_->staged_path),
                              CATALOG_INDEX_STAGING_PATH);
                    runtime_->phase =
                        NightCatalogStoreRuntime::Phase::SubmitWrite;
                    return true;
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
                 runtime_->write_kind != StoreWriteKind::Index &&
                 !runtime_->encoded)) {
                fail("night_catalog_save_not_ready");
                return true;
            }

            StorageAtomicWriteCommand command;
            command.path = runtime_->path;
            if (runtime_->write_kind == StoreWriteKind::InstallSource) {
                command.staged_path = runtime_->staged_path;
                command.staged_size = runtime_->install_size;
            } else if (runtime_->write_kind == StoreWriteKind::Index) {
                if (!runtime_->index_stage_complete ||
                    runtime_->index_size == 0) {
                    fail("night_catalog_save_index_stage_incomplete");
                    return true;
                }
                command.staged_path = runtime_->staged_path;
                command.staged_size = runtime_->index_size;
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
                    : runtime_->write_kind == StoreWriteKind::Index
                        ? runtime_->index_size
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

            runtime_->index_stage_touched = false;
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

        case NightCatalogStoreRuntime::Phase::PrepareIndexChunk: {
            if (!runtime_->index || runtime_->index_size == 0 ||
                runtime_->index_offset >= runtime_->index_size) {
                fail("night_catalog_index_stream_state_invalid");
                return true;
            }

            const size_t remaining =
                runtime_->index_size - runtime_->index_offset;
            const size_t length = std::min(
                remaining, AC_STORAGE_RANGE_WRITE_STEP_BYTES);
            auto chunk = LargeByteBuffer::allocate_shared(length);
            if (!chunk) {
                fail("night_catalog_index_chunk_alloc_failed");
                return true;
            }

            const size_t read =
                runtime_->index_reader.read(chunk->data(), length);
            if (read != length) {
                fail("night_catalog_index_stream_short");
                return true;
            }

            runtime_->index_chunk = std::move(chunk);
            runtime_->index_chunk_size = length;
            runtime_->phase =
                NightCatalogStoreRuntime::Phase::SubmitIndexChunk;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::SubmitIndexChunk: {
            if (!runtime_->index_chunk || runtime_->index_chunk_size == 0 ||
                runtime_->index_offset >= runtime_->index_size) {
                fail("night_catalog_index_chunk_state_invalid");
                return true;
            }

            StorageRangeWriteCommand command;
            command.path = CATALOG_INDEX_STAGING_PATH;
            command.bytes = runtime_->index_chunk;
            command.offset = runtime_->index_offset;
            command.truncate = runtime_->index_offset == 0;
            command.sync_on_close = true;
            command.retain_handle = true;
            command.generation = status_.generation;
            command.lane = StorageAtomicWriteLane::Foreground;

            const OperationSubmission submission =
                range_write_port_->request_write(command);
            if (submission.admission == OperationAdmission::Busy) return false;
            if (!submission.accepted()) {
                fail("night_catalog_index_range_write_rejected");
                return true;
            }

            runtime_->index_stage_touched = true;
            runtime_->range_ticket = submission.ticket;
            runtime_->phase =
                NightCatalogStoreRuntime::Phase::WaitIndexChunk;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::WaitIndexChunk: {
            StorageRangeWriteCompletion completion;
            if (!range_write_port_->take_completion(runtime_->range_ticket,
                                                     completion)) {
                return false;
            }
            runtime_->range_ticket = {};

            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                completion.bytes_written != runtime_->index_chunk_size) {
                fail(completion.error[0]
                         ? completion.error
                         : "night_catalog_index_range_write_failed");
                return true;
            }

            runtime_->index_offset += runtime_->index_chunk_size;
            runtime_->index_chunk.reset();
            runtime_->index_chunk_size = 0;
            runtime_->phase = runtime_->index_offset < runtime_->index_size
                ? NightCatalogStoreRuntime::Phase::PrepareIndexChunk
                : NightCatalogStoreRuntime::Phase::SubmitIndexFinish;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::SubmitIndexFinish: {
            if (runtime_->index_offset != runtime_->index_size) {
                fail("night_catalog_index_finish_before_end");
                return true;
            }

            StorageRangeWriteCommand command;
            command.finish = true;
            command.generation = status_.generation;
            command.lane = StorageAtomicWriteLane::Foreground;

            const OperationSubmission submission =
                range_write_port_->request_write(command);
            if (submission.admission == OperationAdmission::Busy) return false;
            if (!submission.accepted()) {
                fail("night_catalog_index_finish_rejected");
                return true;
            }

            runtime_->range_ticket = submission.ticket;
            runtime_->phase =
                NightCatalogStoreRuntime::Phase::WaitIndexFinish;
            return true;
        }

        case NightCatalogStoreRuntime::Phase::WaitIndexFinish: {
            StorageRangeWriteCompletion completion;
            if (!range_write_port_->take_completion(runtime_->range_ticket,
                                                     completion)) {
                return false;
            }
            runtime_->range_ticket = {};

            if (completion.outcome.disposition !=
                OperationDisposition::Succeeded) {
                fail(completion.error[0]
                         ? completion.error
                         : "night_catalog_index_finish_failed");
                return true;
            }

            runtime_->index_stage_complete = true;
            runtime_->phase = NightCatalogStoreRuntime::Phase::PrepareWrite;
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
    const bool cleanup_index_stage = runtime_->index_stage_touched;
    const bool close_index_handle = cleanup_index_stage &&
                                    !runtime_->index_stage_complete;
    reset_operation();
    if (close_index_handle && range_write_port_) {
        range_write_port_->release_handles();
    }

    if (discard_ledger || (cleanup_index_stage && delete_port_)) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
    }
    if (discard_ledger) {
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

    if (runtime_) runtime_->layout_ready = false;

    if (runtime_ && delete_port_) {
        runtime_->cleanup_pending = true;
        runtime_->staging_root_clean = false;
    }

    stored_.reset();
    published_.reset();
    status_ = {};
}

}  // namespace aircannect
