#include "night_catalog_refresh_service.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#include "edf_file_inventory.h"
#include "edf_report_catalog.h"
#include "edf_report_session.h"
#include "edf_str_file_layout.h"
#include "night_catalog_clock.h"
#include "night_str_record.h"
#include "string_util.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr size_t EDF_HEADER_READ_MAX = 8192;
constexpr size_t SOURCE_READ_BUFFER_BYTES = 64 * 1024;

void *allocate_large(size_t count, size_t size) {
#ifdef ARDUINO
    return Memory::calloc_large(count, size, false);
#else
    return calloc(count, size);
#endif
}

void free_large(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

template <typename T>
void destroy_large_array(T *values, size_t count) {
    if (!values) return;
    for (size_t i = 0; i < count; ++i) values[i].~T();
    free_large(values);
}

template <typename T>
T *allocate_large_array(size_t count) {
    if (count == 0) return nullptr;
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
        return nullptr;
    }

    void *memory = allocate_large(count, sizeof(T));
    if (!memory) return nullptr;

    T *values = static_cast<T *>(memory);
    for (size_t i = 0; i < count; ++i) new (&values[i]) T();
    return values;
}

bool path_is_report_edf(const char *path) {
    EdfInventoryEntry inventory;
    return path && edf_inventory_describe_path(path, inventory) &&
           edf_report_file_kind_supported(inventory.kind);
}

bool same_exact_session(const EdfReportSessionDescriptor &session,
                        const EdfReportFileDescriptor &file) {
    return strcmp(session.sleep_day, file.inventory.sleep_day) == 0 &&
           strcmp(session.session_stamp, file.inventory.session_stamp) == 0;
}

bool add_exact_session_file(EdfReportSessionDescriptor *sessions,
                            size_t capacity,
                            size_t &count,
                            const EdfReportFileDescriptor &file) {
    for (size_t i = 0; i < count; ++i) {
        if (same_exact_session(sessions[i], file)) {
            return edf_report_session_add_file(sessions[i], file);
        }
    }
    if (count >= capacity) return false;

    edf_report_session_init(sessions[count]);
    if (!edf_report_session_add_file(sessions[count], file)) return false;
    ++count;
    return true;
}

bool file_kind(NightCatalogFileKind &out, EdfInventoryFileKind kind) {
    switch (kind) {
        case EdfInventoryFileKind::Brp:
            out = NightCatalogFileKind::Brp;
            return true;
        case EdfInventoryFileKind::Pld:
            out = NightCatalogFileKind::Pld;
            return true;
        case EdfInventoryFileKind::Sa2:
            out = NightCatalogFileKind::Sa2;
            return true;
        case EdfInventoryFileKind::Eve:
            out = NightCatalogFileKind::Eve;
            return true;
        case EdfInventoryFileKind::Csl:
            out = NightCatalogFileKind::Csl;
            return true;
        default:
            return false;
    }
}

void file_signal_masks(const EdfReportSessionDescriptor &session,
                       EdfInventoryFileKind kind,
                       uint32_t &primary_mask,
                       uint32_t &fallback_mask) {
    primary_mask = 0;
    fallback_mask = 0;

    size_t mapping_count = 0;
    const EdfReportSignalMappingDef *mappings =
        edf_report_signal_mapping_defs(mapping_count);
    for (size_t i = 0; i < mapping_count; ++i) {
        const EdfReportSignalMappingDef &mapping = mappings[i];
        if (mapping.kind != kind) continue;

        const uint32_t bit = report_signal_bit(mapping.signal);
        if (mapping.primary && (session.primary_signal_mask & bit) != 0) {
            primary_mask |= bit;
        }
        if (!mapping.primary &&
            (session.fallback_signal_mask & bit) != 0) {
            fallback_mask |= bit;
        }
    }
}

bool resolve_session_clock(EdfReportSessionDescriptor &session,
                           const NightCatalogClockContext &clock,
                           SleepDayId sleep_day) {
    bool resolved = false;
    for (size_t i = 0; i < AC_EDF_REPORT_SESSION_FILE_MAX; ++i) {
        EdfReportSessionFileDescriptor &file = session.files[i];
        if (file.kind == EdfInventoryFileKind::Unknown || !file.path[0] ||
            file.local_header_start_ms <= 0 ||
            file.local_header_end_ms < file.local_header_start_ms) {
            continue;
        }

        if (!night_catalog_resolve_local_time(clock,
                                              sleep_day,
                                              file.local_header_start_ms,
                                              file.header_start_ms) ||
            !night_catalog_resolve_local_time(clock,
                                              sleep_day,
                                              file.local_header_end_ms,
                                              file.header_end_ms)) {
            return false;
        }
        resolved = true;
    }

    if (!resolved) return false;
    edf_report_session_refresh_bounds(session);
    return session.earliest_header_start_ms > 0 &&
           session.latest_header_end_ms >=
               session.earliest_header_start_ms;
}

}  // namespace

struct NightCatalogRefreshRuntime {
    enum class Phase : uint8_t {
        Idle,
        WaitScan,
        SelectEdf,
        WaitEdf,
        SubmitStr,
        WaitStr,
        Build,
        Ready,
        Error,
    };

    ~NightCatalogRefreshRuntime() {
        clear_sources();
        destroy_large_array(summary_records, summary_record_capacity);
        destroy_large_array(summary_sessions, summary_session_capacity);
        free_large(read_buffer);
    }

    void clear_sources() {
        scan.reset();
        destroy_large_array(edf_sessions, edf_session_capacity);
        edf_sessions = nullptr;
        edf_session_capacity = 0;
        edf_session_count = 0;

        destroy_large_array(str_records, str_record_capacity);
        str_records = nullptr;
        str_record_capacity = 0;
        str_record_count = 0;

        scan_index = 0;
        current_path[0] = '\0';
        current_size = 0;
        current_modified = 0;
        str_file_found = false;
        str_file_size = 0;
        str_file_modified = 0;
        str_next_record = 0;
        str_chunk_records = 0;
    }

    void clear_summary() {
        destroy_large_array(summary_records, summary_record_capacity);
        summary_records = nullptr;
        summary_record_capacity = 0;
        summary_record_count = 0;

        destroy_large_array(summary_sessions, summary_session_capacity);
        summary_sessions = nullptr;
        summary_session_capacity = 0;
    }

    Phase phase = Phase::Idle;
    OperationTicket scan_ticket;
    OperationTicket read_ticket;
    StoragePreparedRead prepared;
    std::shared_ptr<const StorageScanSnapshot> scan;

    NightCatalogSummaryInput *summary_records = nullptr;
    size_t summary_record_capacity = 0;
    size_t summary_record_count = 0;
    NightCatalogTimeRange *summary_sessions = nullptr;
    size_t summary_session_capacity = 0;

    EdfReportSessionDescriptor *edf_sessions = nullptr;
    size_t edf_session_capacity = 0;
    size_t edf_session_count = 0;
    NightCatalogStrInput *str_records = nullptr;
    size_t str_record_capacity = 0;
    size_t str_record_count = 0;

    uint8_t *read_buffer = nullptr;
    size_t scan_index = 0;
    char current_path[AC_STORAGE_PATH_MAX] = {};
    uint64_t current_size = 0;
    uint64_t current_modified = 0;

    bool str_file_found = false;
    uint64_t str_file_size = 0;
    uint64_t str_file_modified = 0;
    uint32_t str_next_record = 0;
    uint32_t str_chunk_records = 0;

    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
};

NightCatalogRefreshService::~NightCatalogRefreshService() {
    cancel();
    delete runtime_;
}

void NightCatalogRefreshService::begin(StorageScanPort &scan_port,
                                       StorageReadPort &read_port) {
    if (!runtime_) runtime_ = new (std::nothrow) NightCatalogRefreshRuntime();
    scan_port_ = &scan_port;
    read_port_ = &read_port;
}

bool NightCatalogRefreshService::active() const {
    if (!runtime_) return false;
    return runtime_->phase != NightCatalogRefreshRuntime::Phase::Idle &&
           runtime_->phase != NightCatalogRefreshRuntime::Phase::Ready &&
           runtime_->phase != NightCatalogRefreshRuntime::Phase::Error;
}

void NightCatalogRefreshService::reset_transient() {
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
    runtime_->clear_sources();
    runtime_->clear_summary();
}

void NightCatalogRefreshService::fail(const char *error) {
    if (!runtime_) return;

    reset_transient();
    runtime_->phase = NightCatalogRefreshRuntime::Phase::Error;
    status_.state = NightCatalogRefreshState::Error;
    status_.current_path[0] = '\0';
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "night_catalog_refresh_failed");
}

OperationAdmission NightCatalogRefreshService::request_refresh(
    const NightCatalogSummaryInput *summary_records,
    size_t summary_record_count,
    bool current_offset_valid,
    int32_t current_offset_minutes,
    uint32_t generation) {
    if (!runtime_ || !scan_port_ || !read_port_ || active()) {
        return OperationAdmission::Busy;
    }
    if (generation == 0 ||
        (summary_record_count > 0 && !summary_records)) {
        return OperationAdmission::Rejected;
    }

    reset_transient();

    size_t summary_session_count = 0;
    for (size_t i = 0; i < summary_record_count; ++i) {
        if (!summary_records[i].sleep_day.valid() ||
            summary_records[i].day_end_ms <=
                summary_records[i].day_start_ms ||
            summary_records[i].identity == 0 ||
            (summary_records[i].session_count > 0 &&
             !summary_records[i].sessions) ||
            summary_session_count >
                std::numeric_limits<size_t>::max() -
                    summary_records[i].session_count) {
            return OperationAdmission::Rejected;
        }
        summary_session_count += summary_records[i].session_count;
    }

    runtime_->summary_records =
        allocate_large_array<NightCatalogSummaryInput>(summary_record_count);
    runtime_->summary_record_capacity = summary_record_count;
    runtime_->summary_sessions =
        allocate_large_array<NightCatalogTimeRange>(summary_session_count);
    runtime_->summary_session_capacity = summary_session_count;
    if ((summary_record_count > 0 && !runtime_->summary_records) ||
        (summary_session_count > 0 && !runtime_->summary_sessions)) {
        reset_transient();
        return OperationAdmission::Busy;
    }

    size_t next_session = 0;
    for (size_t i = 0; i < summary_record_count; ++i) {
        runtime_->summary_records[i] = summary_records[i];
        runtime_->summary_records[i].sessions =
            summary_records[i].session_count > 0
                ? runtime_->summary_sessions + next_session
                : nullptr;
        for (size_t j = 0; j < summary_records[i].session_count; ++j) {
            runtime_->summary_sessions[next_session++] =
                summary_records[i].sessions[j];
        }
    }
    runtime_->summary_record_count = summary_record_count;
    runtime_->current_offset_valid = current_offset_valid;
    runtime_->current_offset_minutes = current_offset_minutes;

    StorageScanRoot roots[] = {
        {"/DATALOG", true},
        {"/STR.edf", false},
    };
    StorageScanCommand command;
    command.roots = roots;
    command.root_count = sizeof(roots) / sizeof(roots[0]);
    command.generation = generation;

    const OperationSubmission submission = scan_port_->request_scan(command);
    if (!submission.accepted()) {
        reset_transient();
        return submission.admission;
    }

    runtime_->scan_ticket = submission.ticket;
    runtime_->phase = NightCatalogRefreshRuntime::Phase::WaitScan;
    status_ = {};
    status_.state = NightCatalogRefreshState::Scanning;
    status_.generation = generation;
    return OperationAdmission::Accepted;
}

namespace {

bool prepare_scan_sources(NightCatalogRefreshRuntime &runtime,
                          NightCatalogRefreshStatus &status) {
    size_t report_file_count = 0;
    StorageScanEntryView entry;
    for (size_t i = 0; i < runtime.scan->size(); ++i) {
        if (!runtime.scan->entry(i, entry) || entry.directory) continue;

        if (entry.root_index == 0 && path_is_report_edf(entry.path)) {
            ++report_file_count;
        } else if (entry.root_index == 1 &&
                   strcmp(entry.path, "/STR.edf") == 0) {
            runtime.str_file_found = true;
            runtime.str_file_size = entry.size;
            runtime.str_file_modified = entry.modified;
        }
    }

    runtime.edf_sessions =
        allocate_large_array<EdfReportSessionDescriptor>(report_file_count);
    runtime.edf_session_capacity = report_file_count;
    if (report_file_count > 0 && !runtime.edf_sessions) return false;

    if (!runtime.read_buffer) {
        runtime.read_buffer = static_cast<uint8_t *>(
            allocate_large(1, SOURCE_READ_BUFFER_BYTES));
        if (!runtime.read_buffer) return false;
    }

    status.files_seen = static_cast<uint32_t>(std::min(
        report_file_count,
        static_cast<size_t>(UINT32_MAX)));
    return true;
}

bool submit_next_edf(NightCatalogRefreshRuntime &runtime,
                     StorageReadPort &read_port,
                     NightCatalogRefreshStatus &status) {
    StorageScanEntryView entry;
    while (runtime.scan_index < runtime.scan->size()) {
        const size_t index = runtime.scan_index;
        if (!runtime.scan->entry(index, entry) || entry.directory ||
            entry.root_index != 0 || !path_is_report_edf(entry.path)) {
            ++runtime.scan_index;
            continue;
        }
        if (entry.size < AC_EDF_HEADER_SIGNAL_HEADER_OFFSET ||
            entry.size > SIZE_MAX ||
            strlen(entry.path) >= AC_EDF_REPORT_PATH_MAX ||
            strlen(entry.path) >= sizeof(runtime.current_path)) {
            ++status.files_skipped;
            ++runtime.scan_index;
            continue;
        }

        const size_t read_length = static_cast<size_t>(std::min<uint64_t>(
            entry.size, EDF_HEADER_READ_MAX));
        StorageReadCommand command;
        command.path = entry.path;
        command.length = read_length;
        command.lane = StorageReadLane::Report;
        command.generation = status.generation;

        const OperationSubmission submission = read_port.request_read(command);
        if (submission.admission == OperationAdmission::Busy) return false;
        if (!submission.accepted()) {
            ++status.files_skipped;
            ++runtime.scan_index;
            continue;
        }

        runtime.read_ticket = submission.ticket;
        copy_cstr(runtime.current_path,
                  sizeof(runtime.current_path),
                  entry.path);
        runtime.current_size = entry.size;
        runtime.current_modified = entry.modified;
        copy_cstr(status.current_path,
                  sizeof(status.current_path),
                  entry.path);
        runtime.phase = NightCatalogRefreshRuntime::Phase::WaitEdf;
        return true;
    }

    normalize_edf_report_sessions(runtime.edf_sessions,
                                  runtime.edf_session_count);
    status.sessions = static_cast<uint32_t>(std::min(
        runtime.edf_session_count,
        static_cast<size_t>(UINT32_MAX)));
    runtime.phase = NightCatalogRefreshRuntime::Phase::SubmitStr;
    status.state = NightCatalogRefreshState::ReadingStr;
    status.current_path[0] = '\0';
    return true;
}

void set_warning(NightCatalogRefreshStatus &status, const char *warning) {
    if (!status.warning[0]) {
        copy_cstr(status.warning, sizeof(status.warning), warning);
    }
}

void skip_current_edf(NightCatalogRefreshRuntime &runtime,
                      NightCatalogRefreshStatus &status,
                      const char *warning) {
    ++status.files_skipped;
    ++runtime.scan_index;
    runtime.phase = NightCatalogRefreshRuntime::Phase::SelectEdf;
    runtime.current_path[0] = '\0';
    runtime.current_size = 0;
    runtime.current_modified = 0;
    status.current_path[0] = '\0';
    set_warning(status, warning);
}

bool finish_edf_read(NightCatalogRefreshRuntime &runtime,
                     StorageReadPort &read_port,
                     NightCatalogRefreshStatus &status) {
    StorageReadCompletion completion;
    if (!read_port.take_completion(runtime.read_ticket, completion)) {
        return false;
    }
    runtime.read_ticket = {};

    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid() ||
        completion.prepared.length > SOURCE_READ_BUFFER_BYTES) {
        if (completion.prepared.valid()) {
            read_port.release_prepared(completion.prepared);
        }
        skip_current_edf(runtime,
                         status,
                         "night_catalog_edf_read_failed");
        return true;
    }

    const size_t expected = completion.prepared.length;
    const size_t read = read_port.read_prepared(
        completion.prepared,
        0,
        runtime.read_buffer,
        expected);
    read_port.release_prepared(completion.prepared);
    if (read != expected) {
        skip_current_edf(runtime,
                         status,
                         "night_catalog_edf_read_short");
        return true;
    }

    EdfReportFileDescriptor file;
    const EdfReportFileStatus file_status = edf_report_describe_file(
        runtime.current_path,
        runtime.read_buffer,
        read,
        runtime.current_size,
        static_cast<time_t>(runtime.current_modified),
        0,
        file);
    if (file_status == EdfReportFileStatus::Ok &&
        add_exact_session_file(runtime.edf_sessions,
                               runtime.edf_session_capacity,
                               runtime.edf_session_count,
                               file)) {
        ++status.files_indexed;
    } else {
        ++status.files_skipped;
        set_warning(status, "night_catalog_edf_invalid");
    }

    ++runtime.scan_index;
    runtime.phase = NightCatalogRefreshRuntime::Phase::SelectEdf;
    status.current_path[0] = '\0';
    return true;
}

void skip_str(NightCatalogRefreshRuntime &runtime,
              NightCatalogRefreshStatus &status,
              const char *warning) {
    destroy_large_array(runtime.str_records, runtime.str_record_capacity);
    runtime.str_records = nullptr;
    runtime.str_record_capacity = 0;
    runtime.str_record_count = 0;
    runtime.str_next_record = 0;
    runtime.str_chunk_records = 0;
    runtime.phase = NightCatalogRefreshRuntime::Phase::Build;
    status.state = NightCatalogRefreshState::Building;
    status.str_records = 0;
    status.current_path[0] = '\0';
    ++status.files_skipped;
    set_warning(status, warning);
}

bool prepare_str_records(NightCatalogRefreshRuntime &runtime,
                         NightCatalogRefreshStatus &status) {
    if (!runtime.str_file_found || runtime.str_file_size == 0) {
        runtime.phase = NightCatalogRefreshRuntime::Phase::Build;
        status.state = NightCatalogRefreshState::Building;
        return true;
    }
    if (runtime.str_file_size > SIZE_MAX) {
        skip_str(runtime, status, "night_catalog_str_too_large");
        return true;
    }

    EdfStrFileLayout layout;
    if (!edf_str_file_layout_from_size(
            static_cast<size_t>(runtime.str_file_size), layout)) {
        skip_str(runtime, status, "night_catalog_str_layout_invalid");
        return true;
    }
    if (layout.record_count == 0) {
        runtime.phase = NightCatalogRefreshRuntime::Phase::Build;
        status.state = NightCatalogRefreshState::Building;
        return true;
    }

    runtime.str_records =
        allocate_large_array<NightCatalogStrInput>(layout.record_count);
    runtime.str_record_capacity = layout.record_count;
    if (!runtime.str_records) {
        skip_str(runtime, status, "night_catalog_str_alloc_failed");
        return true;
    }

    return false;
}

bool submit_str_chunk(NightCatalogRefreshRuntime &runtime,
                      StorageReadPort &read_port,
                      NightCatalogRefreshStatus &status) {
    if (runtime.str_next_record >= runtime.str_record_capacity) {
        runtime.phase = NightCatalogRefreshRuntime::Phase::Build;
        status.state = NightCatalogRefreshState::Building;
        status.current_path[0] = '\0';
        return true;
    }

    const size_t record_size = edf_str_record_size();
    if (record_size == 0 || record_size > SOURCE_READ_BUFFER_BYTES) {
        skip_str(runtime,
                 status,
                 "night_catalog_str_record_size_invalid");
        return true;
    }

    const size_t max_chunk_records = SOURCE_READ_BUFFER_BYTES / record_size;
    const size_t remaining =
        runtime.str_record_capacity - runtime.str_next_record;
    runtime.str_chunk_records = static_cast<uint32_t>(
        std::min(remaining, max_chunk_records));

    StorageReadCommand command;
    command.path = "/STR.edf";
    command.offset = edf_str_record_offset(runtime.str_next_record);
    command.length = runtime.str_chunk_records * record_size;
    command.lane = StorageReadLane::Report;
    command.generation = status.generation;

    const OperationSubmission submission = read_port.request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        skip_str(runtime, status, "night_catalog_str_read_rejected");
        return true;
    }

    runtime.read_ticket = submission.ticket;
    runtime.phase = NightCatalogRefreshRuntime::Phase::WaitStr;
    copy_cstr(status.current_path,
              sizeof(status.current_path),
              "/STR.edf");
    return true;
}

bool finish_str_read(NightCatalogRefreshRuntime &runtime,
                     StorageReadPort &read_port,
                     NightCatalogRefreshStatus &status) {
    StorageReadCompletion completion;
    if (!read_port.take_completion(runtime.read_ticket, completion)) {
        return false;
    }
    runtime.read_ticket = {};

    const size_t record_size = edf_str_record_size();
    const size_t expected = runtime.str_chunk_records * record_size;
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid() || completion.prepared.length != expected) {
        if (completion.prepared.valid()) {
            read_port.release_prepared(completion.prepared);
        }
        skip_str(runtime, status, "night_catalog_str_read_failed");
        return true;
    }

    const size_t read = read_port.read_prepared(completion.prepared,
                                                0,
                                                runtime.read_buffer,
                                                expected);
    read_port.release_prepared(completion.prepared);
    if (read != expected) {
        skip_str(runtime, status, "night_catalog_str_read_short");
        return true;
    }

    for (size_t i = 0; i < runtime.str_chunk_records; ++i) {
        NightStrRecord record;
        if (!night_str_record_parse(runtime.read_buffer + i * record_size,
                                    record_size,
                                    record)) {
            continue;
        }

        NightCatalogStrInput &out =
            runtime.str_records[runtime.str_record_count++];
        out.record = record;
        out.path = "/STR.edf";
        out.file_size = runtime.str_file_size;
        out.last_write_ms =
            static_cast<int64_t>(runtime.str_file_modified) * 1000LL;
        out.record_offset = edf_str_record_offset(
            runtime.str_next_record + static_cast<uint32_t>(i));
        out.record_size = static_cast<uint32_t>(record_size);
    }

    runtime.str_next_record += runtime.str_chunk_records;
    status.str_records = static_cast<uint32_t>(std::min(
        runtime.str_record_count,
        static_cast<size_t>(UINT32_MAX)));
    runtime.phase = NightCatalogRefreshRuntime::Phase::SubmitStr;
    return true;
}

bool build_catalog(NightCatalogRefreshRuntime &runtime,
                   std::shared_ptr<const NightCatalog> &catalog,
                   const char *&error) {
    const size_t session_count = runtime.edf_session_count;
    if (session_count >
        std::numeric_limits<size_t>::max() /
            AC_EDF_REPORT_SESSION_FILE_MAX) {
        error = "night_catalog_source_count_overflow";
        return false;
    }

    NightCatalogEdfSessionInput *sessions =
        allocate_large_array<NightCatalogEdfSessionInput>(session_count);
    const size_t file_capacity =
        session_count * AC_EDF_REPORT_SESSION_FILE_MAX;
    NightCatalogSourceFileInput *files =
        allocate_large_array<NightCatalogSourceFileInput>(file_capacity);
    if ((session_count > 0 && !sessions) ||
        (file_capacity > 0 && !files)) {
        destroy_large_array(sessions, session_count);
        destroy_large_array(files, file_capacity);
        error = "night_catalog_build_alloc_failed";
        return false;
    }

    NightCatalogClockContext clock;
    clock.summary_records = runtime.summary_records;
    clock.summary_record_count = runtime.summary_record_count;
    clock.current_offset_valid = runtime.current_offset_valid;
    clock.current_offset_minutes = runtime.current_offset_minutes;

    size_t output_sessions = 0;
    size_t output_files = 0;
    for (size_t i = 0; i < session_count; ++i) {
        EdfReportSessionDescriptor session = runtime.edf_sessions[i];
        SleepDayId sleep_day;
        if (!SleepDayId::from_yyyymmdd(session.sleep_day, sleep_day) ||
            !resolve_session_clock(session, clock, sleep_day)) {
            destroy_large_array(sessions, session_count);
            destroy_large_array(files, file_capacity);
            error = "night_catalog_timezone_unresolved";
            return false;
        }

        NightCatalogEdfSessionInput &out = sessions[output_sessions++];
        out.sleep_day = sleep_day;
        if (!night_catalog_resolve_local_minute(
                &clock, sleep_day, 0, out.day_start_ms) ||
            !night_catalog_resolve_local_minute(
                &clock, sleep_day, 1440, out.day_end_ms)) {
            destroy_large_array(sessions, session_count);
            destroy_large_array(files, file_capacity);
            error = "night_catalog_day_boundary_unresolved";
            return false;
        }

        out.display_window = {session.earliest_header_start_ms,
                              session.latest_header_end_ms};
        out.files = files + output_files;
        for (size_t slot = 0;
             slot < AC_EDF_REPORT_SESSION_FILE_MAX;
             ++slot) {
            const EdfReportSessionFileDescriptor &source =
                session.files[slot];
            const EdfReportSessionFileDescriptor &stored_source =
                runtime.edf_sessions[i].files[slot];
            NightCatalogFileKind kind;
            if (source.kind == EdfInventoryFileKind::Unknown ||
                !source.path[0] || !file_kind(kind, source.kind)) {
                continue;
            }

            NightCatalogSourceFileInput &file = files[output_files++];
            file.kind = kind;
            file.path = stored_source.path;
            file.coverage.range = {source.header_start_ms,
                                   source.header_end_ms};
            file_signal_masks(session,
                              source.kind,
                              file.coverage.primary_signal_mask,
                              file.coverage.fallback_signal_mask);
            file.file_size = source.file_size;
            file.last_write_ms =
                static_cast<int64_t>(source.last_write) * 1000LL;
            file.data_offset = source.header_size;
            file.data_size =
                static_cast<uint64_t>(source.complete_records) *
                source.record_size;
            file.record_start_ms = source.header_start_ms;
            file.header_size = source.header_size;
            file.record_size = source.record_size;
            file.record_duration_ms = source.record_duration_ms;
            file.complete_records = source.complete_records;
            ++out.file_count;
        }
    }

    NightCatalogBuildInput input;
    input.edf_sessions = sessions;
    input.edf_session_count = output_sessions;
    input.str_records = runtime.str_records;
    input.str_record_count = runtime.str_record_count;
    input.summary_records = runtime.summary_records;
    input.summary_record_count = runtime.summary_record_count;
    input.resolve_local_minute = night_catalog_resolve_local_minute;
    input.clock_context = &clock;
    catalog = NightCatalogBuilder::build(input);

    destroy_large_array(sessions, session_count);
    destroy_large_array(files, file_capacity);
    if (!catalog) {
        error = "night_catalog_build_failed";
        return false;
    }
    return true;
}

}  // namespace

bool NightCatalogRefreshService::poll() {
    if (!runtime_ || !scan_port_ || !read_port_) return false;

    const char *error = nullptr;
    switch (runtime_->phase) {
        case NightCatalogRefreshRuntime::Phase::Idle:
        case NightCatalogRefreshRuntime::Phase::Ready:
        case NightCatalogRefreshRuntime::Phase::Error:
            return false;

        case NightCatalogRefreshRuntime::Phase::WaitScan: {
            StorageScanCompletion completion;
            if (!scan_port_->take_completion(runtime_->scan_ticket,
                                              completion)) {
                return false;
            }
            runtime_->scan_ticket = {};
            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                !completion.snapshot ||
                completion.snapshot->generation() != status_.generation) {
                fail(completion.error[0] ? completion.error
                                         : "night_catalog_scan_failed");
                return true;
            }

            runtime_->scan = std::move(completion.snapshot);
            if (!prepare_scan_sources(*runtime_, status_)) {
                fail("night_catalog_source_alloc_failed");
                return true;
            }
            runtime_->phase = NightCatalogRefreshRuntime::Phase::SelectEdf;
            status_.state = NightCatalogRefreshState::ReadingEdf;
            return true;
        }

        case NightCatalogRefreshRuntime::Phase::SelectEdf:
            return submit_next_edf(*runtime_, *read_port_, status_);

        case NightCatalogRefreshRuntime::Phase::WaitEdf:
            if (!finish_edf_read(*runtime_, *read_port_, status_)) {
                return false;
            }
            return true;

        case NightCatalogRefreshRuntime::Phase::SubmitStr:
            if (!runtime_->str_records) {
                const bool finished =
                    prepare_str_records(*runtime_, status_);
                if (finished) return true;
            }
            if (!submit_str_chunk(*runtime_,
                                  *read_port_,
                                  status_)) {
                return false;
            }
            return true;

        case NightCatalogRefreshRuntime::Phase::WaitStr:
            if (!finish_str_read(*runtime_, *read_port_, status_)) {
                return false;
            }
            return true;

        case NightCatalogRefreshRuntime::Phase::Build: {
            std::shared_ptr<const NightCatalog> catalog;
            if (!build_catalog(*runtime_, catalog, error)) {
                fail(error);
                return true;
            }

            published_ = std::move(catalog);
            status_.state = NightCatalogRefreshState::Ready;
            status_.sessions = static_cast<uint32_t>(std::min(
                published_->size(),
                static_cast<size_t>(UINT32_MAX)));
            status_.current_path[0] = '\0';
            status_.error[0] = '\0';
            reset_transient();
            runtime_->phase = NightCatalogRefreshRuntime::Phase::Ready;
            return true;
        }
    }
    return false;
}

void NightCatalogRefreshService::cancel() {
    if (!runtime_) return;

    reset_transient();
    runtime_->phase = published_ ? NightCatalogRefreshRuntime::Phase::Ready
                                 : NightCatalogRefreshRuntime::Phase::Idle;
    status_.state = published_ ? NightCatalogRefreshState::Ready
                               : NightCatalogRefreshState::Idle;
    status_.current_path[0] = '\0';
    status_.error[0] = '\0';
}

}  // namespace aircannect
