#include "storage_export_inventory.h"

#include <limits.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "string_util.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

static constexpr size_t STATE_PARSE_BUDGET_BYTES = 4096;
static constexpr size_t STATE_READ_CHUNK_BYTES = 1024;

void *allocate_inventory(size_t count, size_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) return nullptr;

#ifdef ARDUINO
    return Memory::calloc_large(count, size, false);
#else
    return calloc(count, size);
#endif
}

void free_inventory(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

uint32_t path_hash(const char *path) {
    uint32_t hash = 2166136261u;
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(path ? path : "");
         *p;
         ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

bool path_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) return false;
    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

bool done_path_datalog_day(const char *state_dir,
                           const char *path,
                           char *day_out,
                           size_t day_out_size) {
    if (!state_dir || !state_dir[0] || !path || !day_out ||
        day_out_size < 9) {
        return false;
    }

    const size_t state_dir_length = strlen(state_dir);
    static constexpr char DONE_SUFFIX[] = ".done";
    if (strncmp(path, state_dir, state_dir_length) != 0 ||
        path[state_dir_length] != '/') {
        return false;
    }

    const char *name = path + state_dir_length + 1;
    if (strlen(name) != 8 + sizeof(DONE_SUFFIX) - 1 ||
        !storage_export_all_digits(name, 8) ||
        strcmp(name + 8, DONE_SUFFIX) != 0) {
        return false;
    }

    memcpy(day_out, name, 8);
    day_out[8] = '\0';
    return storage_export_is_datalog_day_name(day_out);
}

}  // namespace

StorageExportInventory::~StorageExportInventory() {
    free_inventory(source_indices_);
    free_inventory(path_index_);
    free_inventory(complete_);
    free_inventory(datalog_days_);
}

size_t StorageExportInventory::source_size() const {
    return source_count_;
}

bool StorageExportInventory::entry(
    size_t source_index,
    StorageExportInventoryEntryView &out) const {
    out = StorageExportInventoryEntryView();
    if (!scan_ || !source_indices_ || source_index >= source_count_) {
        return false;
    }

    StorageScanEntryView source;
    if (!scan_->entry(source_indices_[source_index], source) ||
        source.directory || source.root_index >= export_root_count_) {
        return false;
    }

    out.path = source.path;
    out.info.exists = true;
    out.info.is_dir = false;
    out.info.size = source.size;
    out.info.mtime = source.modified;
    out.source_index = source_index;
    out.root_index = source.root_index;
    out.local_state_complete = complete_ && complete_[source_index] != 0;
    return true;
}

bool StorageExportInventory::find_source_index(
    const char *path,
    size_t &source_index) const {
    if (!path || !path_index_ ||
        source_count_ == 0) {
        return false;
    }

    const uint32_t hash = path_hash(path);
    size_t low = 0;
    size_t high = source_count_;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (path_index_[mid].hash < hash) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    for (size_t i = low;
         i < source_count_ && path_index_[i].hash == hash;
         ++i) {
        StorageExportInventoryEntryView candidate;
        if (entry(path_index_[i].source_index, candidate) &&
            strcmp(candidate.path, path) == 0) {
            source_index = path_index_[i].source_index;
            return true;
        }
    }
    return false;
}

bool StorageExportInventory::find_file(
    const char *path,
    StorageExportInventoryEntryView &out) const {
    size_t source_index = 0;
    if (find_source_index(path, source_index)) {
        return entry(source_index, out);
    }

    out = StorageExportInventoryEntryView();
    return false;
}

const char *StorageExportInventory::datalog_day_at(size_t index) const {
    return index < datalog_day_count_ ? datalog_days_[index].name : nullptr;
}

StorageExportInventory::DatalogDay *StorageExportInventory::find_datalog_day(
    const char *day) {
    return const_cast<DatalogDay *>(
        static_cast<const StorageExportInventory *>(this)->find_datalog_day(
            day));
}

const StorageExportInventory::DatalogDay *
StorageExportInventory::find_datalog_day(const char *day) const {
    if (!day || !datalog_days_) return nullptr;

    size_t low = 0;
    size_t high = datalog_day_count_;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        const int comparison = strcmp(datalog_days_[mid].name, day);
        if (comparison == 0) return &datalog_days_[mid];
        if (comparison > 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return nullptr;
}

bool StorageExportInventory::datalog_day_done(const char *day) const {
    const DatalogDay *entry = find_datalog_day(day);
    return entry && entry->done;
}

bool StorageExportInventory::datalog_day_has_pending(const char *day) const {
    const DatalogDay *entry = find_datalog_day(day);
    return entry && entry->pending_count != 0;
}

const char *StorageExportInventory::latest_datalog_day() const {
    return datalog_day_count_ != 0 ? datalog_days_[0].name : "";
}

bool StorageExportInventory::build_sources() {
    if (!scan_) return false;

    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView source;
        if (scan_->entry(i, source) && !source.directory &&
            source.root_index < export_root_count_) {
            source_count_++;
        }
    }
    if (source_count_ == 0) return true;
    if (source_count_ > UINT32_MAX) return false;

    source_indices_ = static_cast<uint32_t *>(
        allocate_inventory(source_count_, sizeof(uint32_t)));
    path_index_ = static_cast<PathIndexEntry *>(
        allocate_inventory(source_count_, sizeof(PathIndexEntry)));
    complete_ = static_cast<uint8_t *>(
        allocate_inventory(source_count_, sizeof(uint8_t)));
    if (!source_indices_ || !path_index_ || !complete_) return false;

    size_t source_index = 0;
    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView source;
        if (!scan_->entry(i, source) || source.directory ||
            source.root_index >= export_root_count_) {
            continue;
        }

        source_indices_[source_index] = static_cast<uint32_t>(i);
        path_index_[source_index].hash = path_hash(source.path);
        path_index_[source_index].source_index =
            static_cast<uint32_t>(source_index);
        source_index++;
    }
    if (source_index != source_count_) return false;

    qsort(path_index_,
          source_count_,
          sizeof(PathIndexEntry),
          [](const void *left, const void *right) {
              const auto &a = *static_cast<const PathIndexEntry *>(left);
              const auto &b = *static_cast<const PathIndexEntry *>(right);
              if (a.hash < b.hash) return -1;
              if (a.hash > b.hash) return 1;
              if (a.source_index < b.source_index) return -1;
              if (a.source_index > b.source_index) return 1;
              return 0;
          });
    return true;
}

bool StorageExportInventory::build_catalog_days(uint64_t now_epoch) {
    if (!scan_) return false;

    size_t capacity = 0;
    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        char day[9] = {};
        if (scan_->entry(i, entry) && entry.directory &&
            entry.root_index == 0 &&
            storage_export_datalog_day_from_path(entry.path,
                                                 day,
                                                 sizeof(day)) &&
            storage_export_datalog_day_allowed_at(day, now_epoch)) {
            capacity++;
        }
    }
    if (capacity == 0) return true;

    datalog_days_ = static_cast<DatalogDay *>(
        allocate_inventory(capacity, sizeof(DatalogDay)));
    if (!datalog_days_) return false;

    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        char day[9] = {};
        if (!scan_->entry(i, entry) || !entry.directory ||
            entry.root_index != 0 ||
            !storage_export_datalog_day_from_path(entry.path,
                                                  day,
                                                  sizeof(day)) ||
            !storage_export_datalog_day_allowed_at(day, now_epoch)) {
            continue;
        }

        copy_cstr(datalog_days_[datalog_day_count_].name,
                  sizeof(datalog_days_[datalog_day_count_].name),
                  day);
        datalog_day_count_++;
    }

    qsort(datalog_days_,
          datalog_day_count_,
          sizeof(DatalogDay),
          [](const void *left, const void *right) {
              const auto &a = *static_cast<const DatalogDay *>(left);
              const auto &b = *static_cast<const DatalogDay *>(right);
              return strcmp(b.name, a.name);
          });

    size_t unique_count = 0;
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        if (unique_count != 0 &&
            strcmp(datalog_days_[unique_count - 1].name,
                   datalog_days_[i].name) == 0) {
            continue;
        }
        if (unique_count != i) datalog_days_[unique_count] = datalog_days_[i];
        unique_count++;
    }
    datalog_day_count_ = unique_count;

    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        char day[9] = {};
        if (!scan_->entry(i, entry) || entry.directory ||
            entry.root_index != state_root_index_ ||
            !done_path_datalog_day(state_dir_,
                                    entry.path,
                                    day,
                                    sizeof(day))) {
            continue;
        }

        DatalogDay *datalog_day = find_datalog_day(day);
        if (datalog_day) datalog_day->done = true;
    }
    return true;
}

bool StorageExportInventory::build_datalog_day(const char *day,
                                               size_t done_root_index) {
    if (!scan_ || !storage_export_is_datalog_day_name(day)) return false;

    datalog_days_ = static_cast<DatalogDay *>(
        allocate_inventory(1, sizeof(DatalogDay)));
    if (!datalog_days_) return false;

    datalog_day_count_ = 1;
    copy_cstr(datalog_days_[0].name, sizeof(datalog_days_[0].name), day);
    copy_cstr(loaded_datalog_day_, sizeof(loaded_datalog_day_), day);
    datalog_days_[0].file_count = static_cast<uint32_t>(source_count_);
    datalog_days_[0].pending_count = static_cast<uint32_t>(source_count_);

    for (size_t i = 0; i < source_count_; ++i) {
        StorageExportInventoryEntryView source;
        char source_day[9] = {};
        if (!entry(i, source) ||
            !storage_export_datalog_day_from_descendant(source.path,
                                                        source_day,
                                                        sizeof(source_day)) ||
            strcmp(source_day, day) != 0) {
            return false;
        }
    }

    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        if (scan_->entry(i, entry) && !entry.directory &&
            entry.root_index == done_root_index) {
            datalog_days_[0].done = true;
            break;
        }
    }
    return true;
}

std::shared_ptr<StorageExportInventory>
StorageExportInventory::create_catalog(
    std::shared_ptr<const StorageScanSnapshot> scan,
    size_t export_root_count,
    size_t state_root_index,
    const char *state_dir,
    uint64_t now_epoch) {
    if (!scan || export_root_count == 0 || !state_dir || !state_dir[0]) {
        return {};
    }

    std::shared_ptr<StorageExportInventory> inventory(
        new (std::nothrow) StorageExportInventory());
    if (!inventory) return {};

    inventory->scan_ = std::move(scan);
    inventory->export_root_count_ = export_root_count;
    inventory->state_root_index_ = state_root_index;
    inventory->generation_ = inventory->scan_->generation();
    inventory->kind_ = StorageExportInventoryKind::Catalog;
    copy_cstr(inventory->state_dir_,
              sizeof(inventory->state_dir_),
              state_dir);

    if (!inventory->build_sources() ||
        !inventory->build_catalog_days(now_epoch)) {
        return {};
    }
    return inventory;
}

std::shared_ptr<StorageExportInventory>
StorageExportInventory::create_datalog_day(
    std::shared_ptr<const StorageScanSnapshot> scan,
    size_t state_root_index,
    size_t done_root_index,
    const char *state_dir,
    const char *day) {
    if (!scan || !state_dir || !state_dir[0] ||
        !storage_export_is_datalog_day_name(day)) {
        return {};
    }

    std::shared_ptr<StorageExportInventory> inventory(
        new (std::nothrow) StorageExportInventory());
    if (!inventory) return {};

    inventory->scan_ = std::move(scan);
    inventory->export_root_count_ = 1;
    inventory->state_root_index_ = state_root_index;
    inventory->generation_ = inventory->scan_->generation();
    inventory->kind_ = StorageExportInventoryKind::DatalogDay;
    copy_cstr(inventory->state_dir_,
              sizeof(inventory->state_dir_),
              state_dir);

    if (!inventory->build_sources() ||
        !inventory->build_datalog_day(day, done_root_index)) {
        return {};
    }
    return inventory;
}

bool StorageExportInventory::mark_complete(const char *state_path,
                                           const char *local_path,
                                           uint64_t size,
                                           uint64_t mtime) {
    if (!state_path || !local_path || !complete_) return false;

    size_t source_index = 0;
    StorageExportInventoryEntryView candidate;
    if (!find_source_index(local_path, source_index) ||
        !entry(source_index, candidate) || candidate.info.size != size ||
        candidate.info.mtime != mtime) {
        return false;
    }

    char expected_state_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_export_build_state_path(state_dir_,
                                         local_path,
                                         expected_state_path,
                                         sizeof(expected_state_path)) ||
        strcmp(expected_state_path, state_path) != 0) {
        return false;
    }
    if (complete_[source_index] != 0) return true;

    complete_[source_index] = 1;
    char day[9] = {};
    if (storage_export_datalog_day_from_descendant(local_path,
                                                   day,
                                                   sizeof(day))) {
        DatalogDay *entry = find_datalog_day(day);
        if (entry && entry->pending_count != 0) entry->pending_count--;
    }
    return true;
}

StorageExportInventoryLoader::~StorageExportInventoryLoader() {
    reset();
}

void StorageExportInventoryLoader::begin(StorageScanPort &scan_port,
                                         StorageReadPort &read_port) {
    reset();
    scan_port_ = &scan_port;
    read_port_ = &read_port;
}

bool StorageExportInventoryLoader::active() const {
    return phase_ != Phase::Idle && phase_ != Phase::Ready &&
           phase_ != Phase::Error;
}

OperationAdmission StorageExportInventoryLoader::request(
    const char *state_dir,
    uint32_t generation,
    uint64_t now_epoch) {
    if (!scan_port_ || !read_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (!state_dir || !state_dir[0] ||
        strlen(state_dir) >= sizeof(state_dir_) || generation == 0) {
        return OperationAdmission::Rejected;
    }

    reset_operation();
    copy_cstr(state_dir_, sizeof(state_dir_), state_dir);
    generation_ = generation;
    now_epoch_ = now_epoch != 0
        ? now_epoch
        : storage_export_current_epoch_seconds_or_zero();
    request_kind_ = RequestKind::Catalog;

    const size_t export_root_count = storage_export_root_count();
    if (export_root_count + 1 > AC_STORAGE_SCAN_ROOT_MAX) {
        fail("export_scan_root_limit");
        return OperationAdmission::Rejected;
    }

    StorageScanRoot roots[AC_STORAGE_SCAN_ROOT_MAX] = {};
    for (size_t i = 0; i < export_root_count; ++i) {
        const StorageExportRoot &root = storage_export_root_at(i);
        roots[i] = {root.path, i == 0 ? false : root.recursive};
    }
    state_root_index_ = export_root_count;
    roots[state_root_index_] = {state_dir_, false};

    StorageScanCommand command;
    command.roots = roots;
    command.root_count = export_root_count + 1;
    command.include_directories = true;
    command.generation = generation_;
    const OperationSubmission submission = scan_port_->request_scan(command);
    if (!submission.accepted()) {
        phase_ = Phase::Idle;
        request_kind_ = RequestKind::None;
        return submission.admission;
    }

    scan_ticket_ = submission.ticket;
    phase_ = Phase::Scan;
    return OperationAdmission::Accepted;
}

OperationAdmission StorageExportInventoryLoader::request_datalog_day(
    const char *day,
    uint32_t generation) {
    if (!scan_port_ || !read_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (!state_dir_[0] || !storage_export_is_datalog_day_name(day) ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    reset_operation();
    generation_ = generation;
    request_kind_ = RequestKind::DatalogDay;
    copy_cstr(requested_day_, sizeof(requested_day_), day);

    char datalog_path[AC_STORAGE_PATH_MAX] = {};
    char state_path[AC_STORAGE_PATH_MAX] = {};
    char done_path[AC_STORAGE_PATH_MAX] = {};
    const int datalog_length = snprintf(datalog_path,
                                        sizeof(datalog_path),
                                        "/DATALOG/%s",
                                        day);
    if (datalog_length <= 0 ||
        static_cast<size_t>(datalog_length) >= sizeof(datalog_path) ||
        !storage_export_build_state_path(state_dir_,
                                         datalog_path,
                                         state_path,
                                         sizeof(state_path)) ||
        !storage_export_build_done_path(state_dir_,
                                        day,
                                        done_path,
                                        sizeof(done_path))) {
        fail("export_day_path_failed");
        return OperationAdmission::Rejected;
    }

    StorageScanRoot roots[] = {
        {datalog_path, true},
        {state_path, false},
        {done_path, false},
    };
    state_root_index_ = 1;

    StorageScanCommand command;
    command.roots = roots;
    command.root_count = sizeof(roots) / sizeof(roots[0]);
    command.include_directories = false;
    command.generation = generation_;
    const OperationSubmission submission = scan_port_->request_scan(command);
    if (!submission.accepted()) {
        phase_ = Phase::Idle;
        request_kind_ = RequestKind::None;
        return submission.admission;
    }

    scan_ticket_ = submission.ticket;
    phase_ = Phase::Scan;
    return OperationAdmission::Accepted;
}

bool StorageExportInventoryLoader::state_file_matches_request(
    const char *path) const {
    if (!path_has_suffix(path, ".state")) return false;

    char day[9] = {};
    const bool datalog_state = storage_export_state_path_datalog_day(
        state_dir_, path, day, sizeof(day));
    if (request_kind_ == RequestKind::Catalog) return !datalog_state;
    if (request_kind_ == RequestKind::DatalogDay) {
        return datalog_state && strcmp(day, requested_day_) == 0;
    }
    return false;
}

bool StorageExportInventoryLoader::select_next_state_file() {
    while (scan_ && state_scan_index_ < scan_->size()) {
        StorageScanEntryView entry;
        const size_t index = state_scan_index_++;
        if (!scan_->entry(index, entry) || entry.directory ||
            entry.root_index != state_root_index_ || entry.size == 0 ||
            entry.size > SIZE_MAX ||
            !state_file_matches_request(entry.path)) {
            continue;
        }

        copy_cstr(state_path_, sizeof(state_path_), entry.path);
        state_file_size_ = static_cast<size_t>(entry.size);
        return true;
    }

    state_path_[0] = '\0';
    state_file_size_ = 0;
    return false;
}

bool StorageExportInventoryLoader::start_state_read(
    char *error_out,
    size_t error_out_size) {
    if (!state_path_[0] || state_file_size_ == 0) {
        phase_ = Phase::SelectStateFile;
        return true;
    }

    StorageReadCommand command;
    command.path = state_path_;
    command.mode = StorageReadMode::Range;
    command.length = state_file_size_;
    command.lane = StorageReadLane::Export;
    command.generation = generation_;
    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        copy_cstr(error_out, error_out_size, "export_state_read_rejected");
        fail("export_state_read_rejected");
        return false;
    }

    read_ticket_ = submission.ticket;
    phase_ = Phase::ReadStateFile;
    return true;
}

bool StorageExportInventoryLoader::finish_state_read() {
    StorageReadCompletion completion;
    if (!read_port_->take_completion(read_ticket_, completion)) return false;

    read_ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid()) {
        phase_ = Phase::SelectStateFile;
        return true;
    }

    prepared_ = completion.prepared;
    prepared_offset_ = 0;
    state_line_length_ = 0;
    state_line_overflow_ = false;
    phase_ = Phase::ParseStateFile;
    return true;
}

void StorageExportInventoryLoader::parse_state_line() {
    if (!state_line_overflow_) {
        state_line_[state_line_length_] = '\0';

        uint64_t size = 0;
        uint64_t mtime = 0;
        const char *path = nullptr;
        if (storage_export_parse_state_line(state_line_, size, mtime, path)) {
            (void)inventory_->mark_complete(state_path_, path, size, mtime);
        }
    }

    state_line_length_ = 0;
    state_line_overflow_ = false;
}

bool StorageExportInventoryLoader::parse_state_bytes() {
    uint8_t bytes[STATE_READ_CHUNK_BYTES] = {};
    size_t budget = STATE_PARSE_BUDGET_BYTES;

    while (budget > 0 && prepared_offset_ < prepared_.length) {
        const size_t wanted =
            prepared_.length - prepared_offset_ < sizeof(bytes)
                ? prepared_.length - prepared_offset_
                : sizeof(bytes);
        const PreparedByteRead read = read_port_->read_prepared(
            prepared_, prepared_offset_, bytes, wanted);
        if (read.state == PreparedByteReadState::Retry) return false;
        if (read.state != PreparedByteReadState::Data) {
            read_port_->release_prepared(prepared_);
            prepared_ = {};
            phase_ = Phase::SelectStateFile;
            return true;
        }
        prepared_offset_ += read.bytes;
        budget = read.bytes < budget ? budget - read.bytes : 0;

        for (size_t i = 0; i < read.bytes; ++i) {
            const char ch = static_cast<char>(bytes[i]);
            if (ch == '\n') {
                parse_state_line();
            } else if (state_line_overflow_) {
                continue;
            } else if (state_line_length_ + 1 < sizeof(state_line_)) {
                state_line_[state_line_length_++] = ch;
            } else {
                state_line_length_ = 0;
                state_line_overflow_ = true;
            }
        }
    }

    if (prepared_offset_ < prepared_.length) return false;
    if (state_line_length_ != 0) parse_state_line();
    read_port_->release_prepared(prepared_);
    prepared_ = {};
    phase_ = Phase::SelectStateFile;
    return true;
}

void StorageExportInventoryLoader::fail(const char *error) {
    copy_cstr(error_, sizeof(error_), error ? error : "export_inventory_failed");
    phase_ = Phase::Error;
}

StorageExportInventoryLoadResult StorageExportInventoryLoader::poll(
    char *error_out,
    size_t error_out_size) {
    if (phase_ == Phase::Ready) return StorageExportInventoryLoadResult::Ready;
    if (phase_ == Phase::Error) {
        copy_cstr(error_out, error_out_size, error_);
        return StorageExportInventoryLoadResult::Error;
    }
    if (phase_ == Phase::Idle) {
        copy_cstr(error_out, error_out_size, "export_inventory_not_requested");
        return StorageExportInventoryLoadResult::Error;
    }

    if (phase_ == Phase::Scan) {
        StorageScanCompletion completion;
        if (!scan_port_->take_completion(scan_ticket_, completion)) {
            return StorageExportInventoryLoadResult::Waiting;
        }
        scan_ticket_ = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded ||
            !completion.snapshot) {
            fail(completion.error[0] ? completion.error : "export_scan_failed");
            copy_cstr(error_out, error_out_size, error_);
            return StorageExportInventoryLoadResult::Error;
        }

        scan_ = std::move(completion.snapshot);
        if (request_kind_ == RequestKind::Catalog) {
            inventory_ = StorageExportInventory::create_catalog(
                scan_,
                storage_export_root_count(),
                state_root_index_,
                state_dir_,
                now_epoch_);
        } else if (request_kind_ == RequestKind::DatalogDay) {
            inventory_ = StorageExportInventory::create_datalog_day(
                scan_,
                state_root_index_,
                2,
                state_dir_,
                requested_day_);
        }
        if (!inventory_) {
            fail("export_inventory_alloc_failed");
            copy_cstr(error_out, error_out_size, error_);
            return StorageExportInventoryLoadResult::Error;
        }
        phase_ = Phase::SelectStateFile;
    }

    if (phase_ == Phase::SelectStateFile) {
        if (!select_next_state_file()) {
            phase_ = Phase::Ready;
            return StorageExportInventoryLoadResult::Ready;
        }
        if (!start_state_read(error_out, error_out_size)) {
            return phase_ == Phase::Error
                ? StorageExportInventoryLoadResult::Error
                : StorageExportInventoryLoadResult::Waiting;
        }
    }

    if (phase_ == Phase::ReadStateFile && !finish_state_read()) {
        return StorageExportInventoryLoadResult::Waiting;
    }
    if (phase_ == Phase::ParseStateFile && !parse_state_bytes()) {
        return StorageExportInventoryLoadResult::Waiting;
    }
    return phase_ == Phase::Ready ? StorageExportInventoryLoadResult::Ready
                                  : StorageExportInventoryLoadResult::Waiting;
}

void StorageExportInventoryLoader::reset_operation() {
    if (scan_port_ && scan_ticket_.valid()) {
        (void)scan_port_->abandon(scan_ticket_);
    }
    if (read_port_ && read_ticket_.valid()) {
        (void)read_port_->abandon(read_ticket_);
    }
    if (read_port_ && prepared_.valid()) {
        read_port_->release_prepared(prepared_);
    }

    scan_ticket_ = {};
    read_ticket_ = {};
    prepared_ = {};
    scan_.reset();
    inventory_.reset();
    phase_ = Phase::Idle;
    state_root_index_ = 0;
    state_scan_index_ = 0;
    state_file_size_ = 0;
    prepared_offset_ = 0;
    state_line_length_ = 0;
    state_line_overflow_ = false;
    generation_ = 0;
    now_epoch_ = 0;
    request_kind_ = RequestKind::None;
    requested_day_[0] = '\0';
    state_path_[0] = '\0';
    state_line_[0] = '\0';
    error_[0] = '\0';
}

void StorageExportInventoryLoader::reset() {
    reset_operation();
    state_dir_[0] = '\0';
}

}  // namespace aircannect
