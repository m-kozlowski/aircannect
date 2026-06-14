#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_inventory.h"
#include "edf_file_writer.h"

namespace aircannect {

// Session shutdown can enqueue 3 final numeric records, 1 STR day upsert, and
// 5 close jobs before the worker task drains. Keep a little headroom.
static constexpr size_t AC_EDF_STORAGE_QUEUE_CAPACITY = 12;
static constexpr size_t AC_EDF_STORAGE_SLOT_BYTES = 6144;
static constexpr uint32_t AC_EDF_STORAGE_TASK_STACK = 6144;
static constexpr uint8_t AC_EDF_STORAGE_TASK_PRIO = 1;
static constexpr uint8_t AC_EDF_STORAGE_TASK_CORE = 0;
static constexpr uint32_t AC_EDF_STORAGE_IDLE_TICK_MS = 1000;
static constexpr uint32_t AC_EDF_STORAGE_WORK_TICK_MS = 5;
static constexpr size_t AC_EDF_STORAGE_PATIENT_ID_MAX = 80;
static constexpr size_t AC_EDF_STORAGE_RECORDING_ID_MAX = 96;
static constexpr size_t AC_EDF_STORAGE_FILE_COUNT = 5;
static constexpr size_t AC_EDF_STORAGE_INVENTORY_NAME_MAX = 48;
static constexpr size_t AC_EDF_STORAGE_INVENTORY_PATH_MAX = 80;
static constexpr size_t AC_EDF_STORAGE_INVENTORY_HEADER_MAX = 65536;
static constexpr size_t AC_EDF_STORAGE_FILE_BRP = 0;
static constexpr size_t AC_EDF_STORAGE_FILE_PLD = 1;
static constexpr size_t AC_EDF_STORAGE_FILE_SA2 = 2;
static constexpr size_t AC_EDF_STORAGE_FILE_EVE = 3;
static constexpr size_t AC_EDF_STORAGE_FILE_CSL = 4;

struct EdfStorageOpenFileStatus {
    bool open = false;
    bool resumed = false;
    uint32_t record_count = 0;
    char path[80] = {};
};

struct EdfStorageWorkerStatus {
    bool initialized = false;
    bool available = false;
    bool task_started = false;
    bool using_psram = false;
    bool busy = false;
    size_t capacity = 0;
    size_t queued = 0;
    uint8_t open_file_count = 0;
    uint32_t open_jobs = 0;
    uint32_t record_jobs = 0;
    uint32_t close_jobs = 0;
    uint32_t records_written = 0;
    uint32_t queue_drops = 0;
    uint32_t render_errors = 0;
    uint32_t open_errors = 0;
    uint32_t write_errors = 0;
    uint32_t patch_errors = 0;
    uint32_t unavailable_drops = 0;
    uint32_t stack_high_water_words = 0;
    uint64_t bytes_enqueued = 0;
    uint64_t bytes_written = 0;
    uint32_t last_activity_ms = 0;
    char last_path[80] = {};
    char last_error[96] = {};
    EdfStorageOpenFileStatus files[AC_EDF_STORAGE_FILE_COUNT];
};

enum class EdfStorageInventoryStatus : uint8_t {
    Ok,
    BadPath,
    StorageUnavailable,
    Busy,
    NotFound,
    NotDirectory,
    VisitorStopped,
};

struct EdfStorageInventoryEntry {
    bool directory = false;
    uint64_t size = 0;
    char name[AC_EDF_STORAGE_INVENTORY_NAME_MAX] = {};
    char path[AC_EDF_STORAGE_INVENTORY_PATH_MAX] = {};
    EdfInventoryEntry file;
};

struct EdfStorageInventoryResult {
    EdfStorageInventoryStatus status = EdfStorageInventoryStatus::Ok;
    size_t matched = 0;
    size_t returned = 0;
    bool truncated = false;
};

using EdfStorageInventoryVisitor =
    bool (*)(const EdfStorageInventoryEntry &entry, void *ctx);

namespace EdfStorageWorker {

void begin();

bool enqueue_open_numeric(const char *path,
                          const EdfFileSchema &schema,
                          const EdfHeaderInfo &info);
bool enqueue_open_annotation(const char *path,
                             EdfAnnotationKind kind,
                             const EdfHeaderInfo &info);
bool enqueue_numeric_record(const EdfFileSchema &schema,
                            const EdfCompletedRecordView &record);
bool enqueue_annotation_record(EdfAnnotationKind kind,
                               const EdfAnnotationRecord &record);
bool enqueue_str_record(const char *path,
                        const EdfHeaderInfo &info,
                        const EdfStrRecordView &record);
bool enqueue_close_numeric(EdfFileKind kind);
bool enqueue_close_annotation(EdfAnnotationKind kind);

EdfStorageWorkerStatus status();
EdfStorageInventoryResult list_inventory(
    const char *path,
    size_t offset,
    size_t limit,
    EdfStorageInventoryVisitor visitor,
    void *ctx);

const char *inventory_status_name(EdfStorageInventoryStatus status);

}  // namespace EdfStorageWorker
}  // namespace aircannect
