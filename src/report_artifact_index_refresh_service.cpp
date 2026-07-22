#include "report_artifact_index_refresh_service.h"

#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "string_util.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void *allocate_large(size_t count, size_t width) {
    if (count == 0 || width == 0 ||
        count > std::numeric_limits<size_t>::max() / width) {
        return nullptr;
    }
#ifdef ARDUINO
    return Memory::calloc_large(count, width, false);
#else
    return calloc(count, width);
#endif
}

void free_large(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

bool add_size(size_t &value, size_t add) {
    if (value > std::numeric_limits<size_t>::max() - add) return false;
    value += add;
    return true;
}

bool manifest_path_day(const char *path, SleepDayId &day) {
    day = {};
    if (!path) return false;

    const size_t root_length = strlen(REPORT_ARTIFACT_ROOT);
    const size_t path_length = strlen(path);
    constexpr size_t suffix_length = sizeof(".manifest") - 1;
    if (path_length != root_length + 1 + 8 + suffix_length ||
        memcmp(path, REPORT_ARTIFACT_ROOT, root_length) != 0 ||
        path[root_length] != '/' ||
        strcmp(path + path_length - suffix_length, ".manifest") != 0) {
        return false;
    }

    char yyyymmdd[9] = {};
    memcpy(yyyymmdd, path + root_length + 1, 8);
    return SleepDayId::from_yyyymmdd(yyyymmdd, day);
}

bool manifest_size(size_t file_size, size_t &tile_count) {
    tile_count = 0;
    if (file_size < ReportArtifactManifestCodec::HeaderBytes ||
        file_size > ReportArtifactManifestCodec::MaxBytes) {
        return false;
    }

    const size_t body_size =
        file_size - ReportArtifactManifestCodec::HeaderBytes;
    if (body_size % ReportArtifactManifestCodec::TileBytes != 0) {
        return false;
    }

    tile_count = body_size / ReportArtifactManifestCodec::TileBytes;
    return tile_count <= ReportArtifactManifestCodec::MaxTiles;
}

void set_warning(ReportArtifactIndexRefreshStatus &status,
                 const char *warning) {
    if (!status.warning[0]) {
        copy_cstr(status.warning, sizeof(status.warning), warning);
    }
}

}  // namespace

struct ReportArtifactIndexRefreshRuntime {
    enum class Phase : uint8_t {
        Idle,
        WaitScan,
        SelectManifest,
        WaitManifest,
        Build,
        Ready,
        Error,
    };

    ~ReportArtifactIndexRefreshRuntime() {
        clear();
        free_large(read_buffer);
    }

    void clear() {
        scan.reset();
        catalog.reset();
        free_large(inputs);
        free_large(tiles);
        inputs = nullptr;
        tiles = nullptr;
        input_capacity = 0;
        input_count = 0;
        tile_capacity = 0;
        tile_count = 0;
        scan_index = 0;
        current_path[0] = '\0';
        current_size = 0;
    }

    Phase phase = Phase::Idle;
    OperationTicket scan_ticket;
    OperationTicket read_ticket;
    StoragePreparedRead prepared;
    std::shared_ptr<const StorageScanSnapshot> scan;
    std::shared_ptr<const NightCatalog> catalog;

    ReportArtifactIndexInput *inputs = nullptr;
    size_t input_capacity = 0;
    size_t input_count = 0;
    ReportRangeTileArtifact *tiles = nullptr;
    size_t tile_capacity = 0;
    size_t tile_count = 0;
    uint8_t *read_buffer = nullptr;

    size_t scan_index = 0;
    char current_path[AC_STORAGE_PATH_MAX] = {};
    size_t current_size = 0;
};

namespace {

ReportArtifactIndexRefreshRuntime *create_runtime() {
#ifdef ARDUINO
    void *memory = Memory::calloc_large(
        1, sizeof(ReportArtifactIndexRefreshRuntime), false);
    return memory
        ? new (memory) ReportArtifactIndexRefreshRuntime()
        : nullptr;
#else
    return new (std::nothrow) ReportArtifactIndexRefreshRuntime();
#endif
}

void destroy_runtime(ReportArtifactIndexRefreshRuntime *runtime) {
    if (!runtime) return;
#ifdef ARDUINO
    runtime->~ReportArtifactIndexRefreshRuntime();
    Memory::free(runtime);
#else
    delete runtime;
#endif
}

bool prepare_manifest_inputs(ReportArtifactIndexRefreshRuntime &runtime,
                             ReportArtifactIndexRefreshStatus &status) {
    size_t manifest_count = 0;
    size_t tile_capacity = 0;
    StorageScanEntryView entry;
    for (size_t i = 0; i < runtime.scan->size(); ++i) {
        SleepDayId day;
        if (!runtime.scan->entry(i, entry) || entry.directory ||
            entry.root_index != 0 || !manifest_path_day(entry.path, day)) {
            continue;
        }

        ++status.files_seen;
        size_t tile_count = 0;
        if (entry.size > SIZE_MAX ||
            !manifest_size(static_cast<size_t>(entry.size), tile_count)) {
            ++status.files_skipped;
            set_warning(status, "report_artifact_manifest_size_invalid");
            continue;
        }
        if (!add_size(tile_capacity, tile_count)) return false;
        ++manifest_count;
    }

    runtime.inputs = static_cast<ReportArtifactIndexInput *>(
        allocate_large(manifest_count, sizeof(*runtime.inputs)));
    runtime.tiles = static_cast<ReportRangeTileArtifact *>(
        allocate_large(tile_capacity, sizeof(*runtime.tiles)));
    if ((manifest_count > 0 && !runtime.inputs) ||
        (tile_capacity > 0 && !runtime.tiles)) {
        return false;
    }

    if (!runtime.read_buffer) {
        runtime.read_buffer = static_cast<uint8_t *>(allocate_large(
            1, ReportArtifactManifestCodec::MaxBytes));
        if (!runtime.read_buffer) return false;
    }

    runtime.input_capacity = manifest_count;
    runtime.tile_capacity = tile_capacity;
    return true;
}

void skip_manifest(ReportArtifactIndexRefreshRuntime &runtime,
                   ReportArtifactIndexRefreshStatus &status,
                   const char *warning) {
    ++status.files_skipped;
    ++runtime.scan_index;
    runtime.current_path[0] = '\0';
    runtime.current_size = 0;
    runtime.phase =
        ReportArtifactIndexRefreshRuntime::Phase::SelectManifest;
    status.current_path[0] = '\0';
    set_warning(status, warning);
}

bool submit_next_manifest(ReportArtifactIndexRefreshRuntime &runtime,
                          StorageReadPort &read_port,
                          ReportArtifactIndexRefreshStatus &status) {
    StorageScanEntryView entry;
    while (runtime.scan_index < runtime.scan->size()) {
        SleepDayId day;
        if (!runtime.scan->entry(runtime.scan_index, entry) ||
            entry.directory || entry.root_index != 0 ||
            !manifest_path_day(entry.path, day)) {
            ++runtime.scan_index;
            continue;
        }

        size_t tile_count = 0;
        if (entry.size > SIZE_MAX ||
            !manifest_size(static_cast<size_t>(entry.size), tile_count)) {
            ++runtime.scan_index;
            continue;
        }
        if (strlen(entry.path) >= sizeof(runtime.current_path)) {
            skip_manifest(runtime,
                          status,
                          "report_artifact_manifest_path_invalid");
            continue;
        }

        StorageReadCommand command;
        command.path = entry.path;
        command.length = static_cast<size_t>(entry.size);
        command.lane = StorageReadLane::Report;
        command.generation = status.generation;

        const OperationSubmission submission =
            read_port.request_read(command);
        if (submission.admission == OperationAdmission::Busy) return false;
        if (!submission.accepted()) {
            skip_manifest(runtime,
                          status,
                          "report_artifact_manifest_read_rejected");
            continue;
        }

        runtime.read_ticket = submission.ticket;
        runtime.current_size = command.length;
        copy_cstr(runtime.current_path,
                  sizeof(runtime.current_path),
                  entry.path);
        copy_cstr(status.current_path,
                  sizeof(status.current_path),
                  entry.path);
        runtime.phase =
            ReportArtifactIndexRefreshRuntime::Phase::WaitManifest;
        return true;
    }

    runtime.phase = ReportArtifactIndexRefreshRuntime::Phase::Build;
    status.state = ReportArtifactIndexRefreshState::Building;
    status.current_path[0] = '\0';
    return true;
}

bool finish_manifest_read(ReportArtifactIndexRefreshRuntime &runtime,
                          StorageReadPort &read_port,
                          ReportArtifactIndexRefreshStatus &status) {
    if (!runtime.prepared.valid()) {
        StorageReadCompletion completion;
        if (!read_port.take_completion(runtime.read_ticket, completion)) {
            return false;
        }
        runtime.read_ticket = {};

        if (completion.outcome.disposition !=
                OperationDisposition::Succeeded ||
            !completion.prepared.valid() ||
            completion.prepared.length != runtime.current_size) {
            if (completion.prepared.valid()) {
                read_port.release_prepared(completion.prepared);
            }
            skip_manifest(runtime,
                          status,
                          "report_artifact_manifest_read_failed");
            return true;
        }
        runtime.prepared = completion.prepared;
    }

    const PreparedByteRead read = read_port.read_prepared(
        runtime.prepared, 0, runtime.read_buffer, runtime.current_size);
    if (read.state == PreparedByteReadState::Retry) return false;

    read_port.release_prepared(runtime.prepared);
    runtime.prepared = {};
    if (read.state != PreparedByteReadState::Data ||
        read.bytes != runtime.current_size) {
        skip_manifest(runtime,
                      status,
                      "report_artifact_manifest_read_short");
        return true;
    }

    ReportArtifactManifestView manifest;
    SleepDayId path_day;
    const NightCatalogRecord *night = nullptr;
    const bool valid = manifest_path_day(runtime.current_path, path_day) &&
        ReportArtifactManifestCodec::decode(
            runtime.read_buffer, read.bytes, manifest) &&
        manifest.key.sleep_day == path_day && runtime.catalog &&
        (night = runtime.catalog->find(path_day)) != nullptr &&
        night->source_revision == manifest.key.source_revision &&
        runtime.input_count < runtime.input_capacity &&
        runtime.tile_count <= runtime.tile_capacity &&
        manifest.tile_count <= runtime.tile_capacity - runtime.tile_count;
    if (!valid) {
        skip_manifest(runtime,
                      status,
                      "report_artifact_manifest_invalid_or_stale");
        return true;
    }

    const size_t first_tile = runtime.tile_count;
    for (size_t i = 0; i < manifest.tile_count; ++i) {
        if (!manifest.tile(i, runtime.tiles[runtime.tile_count++])) {
            runtime.tile_count = first_tile;
            skip_manifest(runtime,
                          status,
                          "report_artifact_manifest_tile_invalid");
            return true;
        }
    }

    ReportArtifactIndexInput &input = runtime.inputs[runtime.input_count++];
    input.key = manifest.key;
    input.result_size = manifest.result_size;
    input.overview_size = manifest.overview_size;
    input.result_crc32 = manifest.result_crc32;
    input.overview_crc32 = manifest.overview_crc32;
    input.tiles = manifest.tile_count > 0
        ? runtime.tiles + first_tile
        : nullptr;
    input.tile_count = manifest.tile_count;

    ++status.files_indexed;
    ++runtime.scan_index;
    runtime.current_path[0] = '\0';
    runtime.current_size = 0;
    runtime.phase =
        ReportArtifactIndexRefreshRuntime::Phase::SelectManifest;
    status.current_path[0] = '\0';
    return true;
}

}  // namespace

ReportArtifactIndexRefreshService::~ReportArtifactIndexRefreshService() {
    cancel();
    destroy_runtime(runtime_);
}

void ReportArtifactIndexRefreshService::begin(
    StorageScanPort &scan_port,
    StorageReadPort &read_port) {
    if (!runtime_) runtime_ = create_runtime();
    scan_port_ = &scan_port;
    read_port_ = &read_port;
}

bool ReportArtifactIndexRefreshService::active() const {
    if (!runtime_) return false;
    return runtime_->phase != ReportArtifactIndexRefreshRuntime::Phase::Idle &&
           runtime_->phase != ReportArtifactIndexRefreshRuntime::Phase::Ready &&
           runtime_->phase != ReportArtifactIndexRefreshRuntime::Phase::Error;
}

void ReportArtifactIndexRefreshService::reset_transient() {
    if (!runtime_) return;

    if (scan_port_ && runtime_->scan_ticket.valid()) {
        (void)scan_port_->abandon(runtime_->scan_ticket);
    }
    if (read_port_ && runtime_->read_ticket.valid()) {
        (void)read_port_->abandon(runtime_->read_ticket);
    }
    if (read_port_ && runtime_->prepared.valid()) {
        read_port_->release_prepared(runtime_->prepared);
    }
    runtime_->scan_ticket = {};
    runtime_->read_ticket = {};
    runtime_->prepared = {};
    runtime_->clear();
}

void ReportArtifactIndexRefreshService::fail(const char *error) {
    if (!runtime_) return;

    reset_transient();
    runtime_->phase = ReportArtifactIndexRefreshRuntime::Phase::Error;
    status_.state = ReportArtifactIndexRefreshState::Error;
    status_.current_path[0] = '\0';
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "report_artifact_index_refresh_failed");
}

OperationAdmission ReportArtifactIndexRefreshService::request_refresh(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (!runtime_ || !scan_port_ || !read_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (!catalog || generation == 0) return OperationAdmission::Rejected;

    reset_transient();
    runtime_->catalog = std::move(catalog);

    const StorageScanRoot root{REPORT_ARTIFACT_ROOT, false};
    StorageScanCommand command;
    command.roots = &root;
    command.root_count = 1;
    command.generation = generation;

    const OperationSubmission submission = scan_port_->request_scan(command);
    if (!submission.accepted()) {
        reset_transient();
        return submission.admission;
    }

    runtime_->scan_ticket = submission.ticket;
    runtime_->phase = ReportArtifactIndexRefreshRuntime::Phase::WaitScan;
    status_ = {};
    status_.state = ReportArtifactIndexRefreshState::Scanning;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

bool ReportArtifactIndexRefreshService::poll() {
    if (!runtime_ || !scan_port_ || !read_port_) return false;
    ReportArtifactIndexRefreshRuntime &runtime = *runtime_;

    switch (runtime.phase) {
        case ReportArtifactIndexRefreshRuntime::Phase::WaitScan: {
            StorageScanCompletion completion;
            if (!scan_port_->take_completion(runtime.scan_ticket,
                                              completion)) {
                return false;
            }
            runtime.scan_ticket = {};
            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                !completion.snapshot ||
                completion.snapshot->generation() != status_.generation) {
                fail("report_artifact_manifest_scan_failed");
                return true;
            }

            runtime.scan = std::move(completion.snapshot);
            if (!prepare_manifest_inputs(runtime, status_)) {
                fail("report_artifact_index_allocation_failed");
                return true;
            }
            runtime.phase =
                ReportArtifactIndexRefreshRuntime::Phase::SelectManifest;
            status_.state = ReportArtifactIndexRefreshState::Reading;
            return true;
        }

        case ReportArtifactIndexRefreshRuntime::Phase::SelectManifest:
            return submit_next_manifest(runtime, *read_port_, status_);

        case ReportArtifactIndexRefreshRuntime::Phase::WaitManifest:
            return finish_manifest_read(runtime, *read_port_, status_);

        case ReportArtifactIndexRefreshRuntime::Phase::Build: {
            std::shared_ptr<const ReportArtifactIndex> built =
                ReportArtifactIndexBuilder::build(runtime.inputs,
                                                  runtime.input_count);
            if (!built) {
                fail("report_artifact_index_build_failed");
                return true;
            }

            published_ = std::move(built);
            reset_transient();
            runtime.phase = ReportArtifactIndexRefreshRuntime::Phase::Ready;
            status_.state = ReportArtifactIndexRefreshState::Ready;
            status_.current_path[0] = '\0';
            return true;
        }

        case ReportArtifactIndexRefreshRuntime::Phase::Idle:
        case ReportArtifactIndexRefreshRuntime::Phase::Ready:
        case ReportArtifactIndexRefreshRuntime::Phase::Error:
            return false;
    }
    return false;
}

void ReportArtifactIndexRefreshService::cancel() {
    if (!runtime_) return;

    reset_transient();
    runtime_->phase = ReportArtifactIndexRefreshRuntime::Phase::Idle;
    status_ = {};
}

}  // namespace aircannect
