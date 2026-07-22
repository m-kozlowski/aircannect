#include "storage_export_inventory.h"

#include <limits.h>
#include <new>
#include <string.h>

#include "memory_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t STATE_PARSE_BUDGET_BYTES = 4096;
static constexpr size_t STATE_READ_CHUNK_BYTES = 1024;

bool path_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) return false;
    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

}  // namespace

StorageExportInventory::~StorageExportInventory() {
    if (complete_) Memory::free(complete_);
    if (datalog_days_) Memory::free(datalog_days_);
}

size_t StorageExportInventory::source_size() const {
    return scan_ ? scan_->size() : 0;
}

bool StorageExportInventory::entry(
    size_t source_index,
    StorageExportInventoryEntryView &out) const {
    out = StorageExportInventoryEntryView();
    if (!scan_ || source_index >= scan_->size()) return false;

    StorageScanEntryView source;
    if (!scan_->entry(source_index, source) || source.directory ||
        source.root_index >= export_root_count_) {
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

bool StorageExportInventory::find_file(
    const char *path,
    StorageExportInventoryEntryView &out) const {
    if (!path) return false;
    for (size_t i = 0; i < source_size(); ++i) {
        StorageExportInventoryEntryView candidate;
        if (entry(i, candidate) && strcmp(candidate.path, path) == 0) {
            out = candidate;
            return true;
        }
    }
    out = StorageExportInventoryEntryView();
    return false;
}

const char *StorageExportInventory::datalog_day_at(size_t index) const {
    return index < datalog_day_count_ ? datalog_days_[index].name : nullptr;
}

bool StorageExportInventory::datalog_day_done(const char *day) const {
    if (!day) return false;
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        if (strcmp(datalog_days_[i].name, day) == 0) {
            return datalog_days_[i].done;
        }
    }
    return false;
}

bool StorageExportInventory::datalog_day_has_pending(const char *day) const {
    if (!day) return false;
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        if (strcmp(datalog_days_[i].name, day) == 0) {
            return datalog_days_[i].pending_count != 0;
        }
    }
    return false;
}

const char *StorageExportInventory::latest_datalog_day() const {
    return datalog_day_count_ != 0 ? datalog_days_[0].name : "";
}

bool StorageExportInventory::scan_path_exists(size_t root_index,
                                              const char *path) const {
    if (!scan_ || !path) return false;
    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        if (scan_->entry(i, entry) && !entry.directory &&
            entry.root_index == root_index && strcmp(entry.path, path) == 0) {
            return true;
        }
    }
    return false;
}

bool StorageExportInventory::build_datalog_days() {
    if (!scan_) return false;

    size_t capacity = 0;
    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        char day[9] = {};
        if (scan_->entry(i, entry) && entry.directory &&
            entry.root_index == 0 &&
            storage_export_datalog_day_from_path(entry.path,
                                                 day,
                                                 sizeof(day))) {
            capacity++;
        }
    }
    if (capacity == 0) return true;

    datalog_days_ = static_cast<DatalogDay *>(
        Memory::calloc_large(capacity, sizeof(DatalogDay), false));
    if (!datalog_days_) return false;

    for (size_t i = 0; i < scan_->size(); ++i) {
        StorageScanEntryView entry;
        char day[9] = {};
        if (!scan_->entry(i, entry) || !entry.directory ||
            entry.root_index != 0 ||
            !storage_export_datalog_day_from_path(entry.path,
                                                  day,
                                                  sizeof(day))) {
            continue;
        }

        size_t position = 0;
        while (position < datalog_day_count_ &&
               strcmp(datalog_days_[position].name, day) > 0) {
            position++;
        }
        if (position < datalog_day_count_ &&
            strcmp(datalog_days_[position].name, day) == 0) {
            continue;
        }
        for (size_t j = datalog_day_count_; j > position; --j) {
            datalog_days_[j] = datalog_days_[j - 1];
        }
        copy_cstr(datalog_days_[position].name,
                  sizeof(datalog_days_[position].name),
                  day);
        datalog_day_count_++;
    }

    for (size_t i = 0; i < datalog_day_count_; ++i) {
        char done_path[AC_STORAGE_PATH_MAX] = {};
        if (storage_export_build_done_path(state_dir_,
                                           datalog_days_[i].name,
                                           done_path,
                                           sizeof(done_path))) {
            datalog_days_[i].done =
                scan_path_exists(state_root_index_, done_path);
        }
    }

    for (size_t i = 0; i < source_size(); ++i) {
        StorageExportInventoryEntryView file;
        char day[9] = {};
        if (!entry(i, file) ||
            !storage_export_datalog_day_from_descendant(file.path,
                                                        day,
                                                        sizeof(day))) {
            continue;
        }
        for (size_t j = 0; j < datalog_day_count_; ++j) {
            if (strcmp(datalog_days_[j].name, day) != 0) continue;
            datalog_days_[j].file_count++;
            datalog_days_[j].pending_count++;
            break;
        }
    }
    return true;
}

std::shared_ptr<StorageExportInventory> StorageExportInventory::create(
    std::shared_ptr<const StorageScanSnapshot> scan,
    size_t export_root_count,
    size_t state_root_index,
    const char *state_dir) {
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
    copy_cstr(inventory->state_dir_,
              sizeof(inventory->state_dir_),
              state_dir);

    const size_t source_size = inventory->scan_->size();
    if (source_size != 0) {
        inventory->complete_ = static_cast<uint8_t *>(
            Memory::calloc_large(source_size, sizeof(uint8_t), false));
        if (!inventory->complete_) return {};
    }
    if (!inventory->build_datalog_days()) return {};
    return inventory;
}

bool StorageExportInventory::mark_complete(const char *state_path,
                                           const char *local_path,
                                           uint64_t size,
                                           uint64_t mtime) {
    if (!state_path || !local_path || !complete_) return false;

    for (size_t i = 0; i < source_size(); ++i) {
        StorageExportInventoryEntryView candidate;
        if (!entry(i, candidate) || candidate.info.size != size ||
            candidate.info.mtime != mtime ||
            strcmp(candidate.path, local_path) != 0) {
            continue;
        }

        char expected_state_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_export_build_state_path(state_dir_,
                                             local_path,
                                             expected_state_path,
                                             sizeof(expected_state_path)) ||
            strcmp(expected_state_path, state_path) != 0) {
            return false;
        }
        if (complete_[i] != 0) return true;

        complete_[i] = 1;
        char day[9] = {};
        if (storage_export_datalog_day_from_descendant(local_path,
                                                       day,
                                                       sizeof(day))) {
            for (size_t j = 0; j < datalog_day_count_; ++j) {
                DatalogDay &entry = datalog_days_[j];
                if (strcmp(entry.name, day) == 0 && entry.pending_count != 0) {
                    entry.pending_count--;
                    break;
                }
            }
        }
        return true;
    }
    return false;
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
    uint32_t generation) {
    if (!scan_port_ || !read_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (!state_dir || !state_dir[0] ||
        strlen(state_dir) >= sizeof(state_dir_) || generation == 0) {
        return OperationAdmission::Rejected;
    }

    reset();
    copy_cstr(state_dir_, sizeof(state_dir_), state_dir);
    generation_ = generation;

    const size_t export_root_count = storage_export_root_count();
    if (export_root_count + 1 > AC_STORAGE_SCAN_ROOT_MAX) {
        fail("export_scan_root_limit");
        return OperationAdmission::Rejected;
    }

    StorageScanRoot roots[AC_STORAGE_SCAN_ROOT_MAX] = {};
    for (size_t i = 0; i < export_root_count; ++i) {
        const StorageExportRoot &root = storage_export_root_at(i);
        roots[i] = {root.path, root.recursive};
    }
    state_root_index_ = export_root_count;
    roots[state_root_index_] = {state_dir_, true};

    StorageScanCommand command;
    command.roots = roots;
    command.root_count = export_root_count + 1;
    command.include_directories = true;
    command.generation = generation_;
    const OperationSubmission submission = scan_port_->request_scan(command);
    if (!submission.accepted()) {
        phase_ = Phase::Idle;
        return submission.admission;
    }

    scan_ticket_ = submission.ticket;
    phase_ = Phase::Scan;
    return OperationAdmission::Accepted;
}

bool StorageExportInventoryLoader::select_next_state_file() {
    while (scan_ && state_scan_index_ < scan_->size()) {
        StorageScanEntryView entry;
        const size_t index = state_scan_index_++;
        if (!scan_->entry(index, entry) || entry.directory ||
            entry.root_index != state_root_index_ ||
            !path_has_suffix(entry.path, ".state")) {
            continue;
        }
        if (entry.size == 0 || entry.size > SIZE_MAX) continue;

        copy_cstr(state_path_, sizeof(state_path_), entry.path);
        return true;
    }
    state_path_[0] = '\0';
    return false;
}

bool StorageExportInventoryLoader::start_state_read(char *error_out,
                                                    size_t error_out_size) {
    StorageScanEntryView entry;
    bool found = false;
    for (size_t i = 0; scan_ && i < scan_->size(); ++i) {
        if (scan_->entry(i, entry) && !entry.directory &&
            entry.root_index == state_root_index_ &&
            strcmp(entry.path, state_path_) == 0) {
            found = true;
            break;
        }
    }
    if (!found || entry.size == 0 || entry.size > SIZE_MAX) {
        phase_ = Phase::SelectStateFile;
        return true;
    }

    StorageReadCommand command;
    command.path = state_path_;
    command.mode = StorageReadMode::Range;
    command.length = static_cast<size_t>(entry.size);
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
        inventory_ = StorageExportInventory::create(scan_,
                                                    storage_export_root_count(),
                                                    state_root_index_,
                                                    state_dir_);
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

void StorageExportInventoryLoader::reset() {
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
    prepared_offset_ = 0;
    state_line_length_ = 0;
    state_line_overflow_ = false;
    generation_ = 0;
    state_dir_[0] = '\0';
    state_path_[0] = '\0';
    state_line_[0] = '\0';
    error_[0] = '\0';
}

}  // namespace aircannect
