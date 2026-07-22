#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "edf_file_writer.h"
#include "runtime_snapshots.h"
#include "storage_archive_port.h"
#include "storage_browser_port.h"
#include "storage_delete_port.h"
#include "storage_read_port.h"

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
static constexpr size_t AC_STORAGE_PREPARED_READ_MAX_BYTES = 512 * 1024;
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

struct StorageFileLogStatus {
    bool available = false;
    bool enabled = false;
    bool open = false;
    size_t queue_capacity = 0;
    size_t queued = 0;
    uint64_t bytes = 0;
    uint32_t written = 0;
    uint32_t drops = 0;
    uint32_t errors = 0;
};

namespace StorageService {

// lifecycle
void begin();

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

// Foreground storage browsing and downloads
StorageBrowserPort &browser_port();

// Background storage maintenance
StorageArchivePort &archive_port();
StorageDeletePort &delete_port();

// File logging
bool configure_file_log(bool enabled);
bool enqueue_file_log_line(const char *line, size_t length);
StorageFileLogStatus file_log_status();

// Runtime facts used by storage-owned scheduling policy
void publish_activity(const ActivitySnapshot &activity);

// Status
StorageServiceStatus status();
#if AC_STACK_PROFILE_ENABLED
uint32_t stack_high_water_bytes();
#endif

}  // namespace StorageService
}  // namespace aircannect
