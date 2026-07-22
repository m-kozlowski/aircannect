#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "edf_file_writer.h"
#include "file_log_sink_port.h"
#include "runtime_snapshots.h"
#include "storage_archive_port.h"
#include "storage_atomic_write_port.h"
#include "storage_browser_port.h"
#include "storage_delete_port.h"
#include "storage_path_port.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"
#include "storage_stream_port.h"
#include "storage_upload_port.h"

namespace aircannect {

// Session shutdown can enqueue 3 final numeric records, 1 STR day upsert,
// 1 metadata write, and 5 close jobs before the worker task drains.
// Keep a little headroom.
static constexpr size_t AC_EDF_STORAGE_QUEUE_CAPACITY = 12;
static constexpr size_t AC_EDF_STORAGE_SLOT_BYTES = 6144;

static constexpr uint32_t AC_STORAGE_SERVICE_TASK_STACK = 6144;
static constexpr uint8_t AC_STORAGE_SERVICE_TASK_PRIO = 1;
static constexpr uint8_t AC_STORAGE_SERVICE_TASK_CORE = 0;
static constexpr uint32_t AC_STORAGE_SERVICE_IDLE_TICK_MS = 1000;
static constexpr uint32_t AC_STORAGE_SERVICE_WORK_TICK_MS = 5;

static constexpr size_t AC_STORAGE_PREPARED_READ_CAPACITY = 4;
static constexpr size_t AC_STORAGE_READ_STEP_BYTES = 4096;

static constexpr size_t AC_EDF_STORAGE_PATIENT_ID_MAX = 80;
static constexpr size_t AC_EDF_STORAGE_RECORDING_ID_MAX = 96;

enum class EdfStorageFileIndex : uint8_t {
    Brp,
    Pld,
    Sa2,
    Eve,
    Csl,
    Count,
};

static constexpr size_t AC_EDF_STORAGE_FILE_COUNT =
    static_cast<size_t>(EdfStorageFileIndex::Count);

constexpr size_t edf_storage_file_index(EdfStorageFileIndex index) {
    return static_cast<size_t>(index);
}

struct EdfStorageOpenFileStatus {
    bool open = false;
    bool resumed = false;
    uint32_t record_count = 0;
    char path[80] = {};
};

struct EdfStorageOpenHandle {
    EdfStorageFileIndex file = EdfStorageFileIndex::Count;
    uint32_t request_id = 0;

    bool valid() const {
        return file != EdfStorageFileIndex::Count && request_id != 0;
    }
};

struct EdfStorageOpenResult {
    bool complete = false;
    bool success = false;
    bool superseded = false;
    bool open = false;
    bool resumed = false;
    uint32_t record_count = 0;
    char path[80] = {};
    char error[96] = {};
};

struct StorageServiceStatus {
    bool initialized = false;
    bool available = false;
    bool task_started = false;
    bool using_psram = false;
    bool busy = false;
    bool maintenance_active = false;

    size_t edf_capacity = 0;
    size_t edf_queued = 0;
    size_t read_capacity = 0;
    size_t read_queued = 0;
    size_t prepared_reads = 0;
    uint8_t open_file_count = 0;

    uint32_t open_jobs = 0;
    uint32_t record_jobs = 0;
    uint32_t close_jobs = 0;
    uint32_t identification_jobs = 0;
    uint32_t records_written = 0;

    uint32_t queue_drops = 0;
    uint32_t render_errors = 0;
    uint32_t open_errors = 0;
    uint32_t write_errors = 0;
    uint32_t patch_errors = 0;
    uint32_t unavailable_drops = 0;
    uint32_t read_jobs = 0;
    uint32_t read_errors = 0;
    uint32_t read_cancellations = 0;

#if AC_STACK_PROFILE_ENABLED
    uint32_t stack_high_water_words = 0;
#endif
    uint64_t bytes_enqueued = 0;
    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint32_t last_activity_ms = 0;

    char last_path[80] = {};
    char last_error[96] = {};

    EdfStorageOpenFileStatus files[AC_EDF_STORAGE_FILE_COUNT];
};

struct StorageWorkloadSnapshot {
    bool valid = false;
    bool available = false;
    bool busy = true;
    bool maintenance_active = false;
    size_t edf_queued = 0;
    uint8_t open_file_count = 0;
};

struct StorageEdfStatusSnapshot {
    bool busy = true;
    size_t capacity = 0;
    size_t queued = 0;
    uint8_t open_file_count = 0;
    uint32_t records_written = 0;
    uint32_t identification_jobs = 0;
    uint32_t queue_drops = 0;
    uint32_t patch_errors = 0;
#if AC_STACK_PROFILE_ENABLED
    uint32_t stack_high_water_words = 0;
#endif
    char last_error[AC_STORAGE_ERROR_MAX] = {};
};

class StorageStatusPort {
public:
    virtual ~StorageStatusPort() = default;

    virtual bool mounted() const = 0;
    virtual StorageWorkloadSnapshot workload_snapshot() const = 0;
};

enum class StorageDiagnosticState : uint8_t {
    Idle,
    Queued,
    Writing,
    Complete,
    Failed,
};

struct StorageDiagnosticStatus {
    bool available = false;
    StorageDiagnosticState state = StorageDiagnosticState::Idle;
    size_t bytes = 0;
    char path[AC_STORAGE_WRITE_PATH_MAX] = {};
    char error[64] = {};
};

const char *storage_diagnostic_state_name(StorageDiagnosticState state);

namespace StorageService {

// lifecycle
void begin();
bool request_mount_retry();

// EDF file opens
bool enqueue_edf_open_numeric(const char *path,
                              const EdfFileSchema &schema,
                              const EdfHeaderInfo &info,
                              EdfStorageOpenHandle *handle = nullptr);
bool enqueue_edf_open_annotation(const char *path,
                                 EdfAnnotationKind kind,
                                 const EdfHeaderInfo &info,
                                 EdfStorageOpenHandle *handle = nullptr);

// EDF record writes
bool enqueue_edf_numeric_record(const EdfFileSchema &schema,
                                const EdfCompletedRecordView &record);
bool enqueue_edf_annotation_record(EdfAnnotationKind kind,
                                   const EdfAnnotationRecord &record);
bool enqueue_edf_str_record(const char *path,
                            const EdfHeaderInfo &info,
                            const EdfStrRecordView &record);

// EDF metadata and closes
bool enqueue_edf_identification_files(const std::string &json);
bool enqueue_edf_close_numeric(EdfFileKind kind);
bool enqueue_edf_close_annotation(EdfAnnotationKind kind);
bool edf_open_result(const EdfStorageOpenHandle &handle,
                     EdfStorageOpenResult &result);

// Prepared bounded reads
StorageReadPort &read_port();

// Sequential storage-owned byte streams
StorageStreamPort &stream_port();

// File metadata and path mutations
StoragePathPort &path_port();

// Atomic immutable file publication
StorageAtomicWritePort &atomic_write_port();

// Foreground storage browsing and downloads
StorageBrowserPort &browser_port();

// Bounded inbound file uploads
StorageUploadPort &upload_port();
bool take_uploaded_path(char *path, size_t path_size);

// Background storage maintenance
StorageScanPort &scan_port();
StorageArchivePort &archive_port();
StorageDeletePort &delete_port();

// Diagnostic writes
bool request_diagnostic_append(const char *path,
                               const uint8_t *data,
                               size_t length);
StorageDiagnosticStatus diagnostic_status();

// File logging producer
FileLogSinkPort &file_log_port();

// Runtime facts used by storage-owned scheduling policy
void publish_activity(const ActivitySnapshot &activity);

// Status
StorageStatusPort &status_port();
StorageWorkloadSnapshot workload_snapshot();
StorageEdfStatusSnapshot edf_status_snapshot();
#if AC_STACK_PROFILE_ENABLED
uint32_t stack_high_water_bytes();
#endif

}  // namespace StorageService
}  // namespace aircannect
