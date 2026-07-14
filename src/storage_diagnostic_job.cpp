#include "storage_diagnostic_job.h"

#include <FS.h>
#include <string.h>

#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {

const char *storage_diagnostic_state_name(StorageDiagnosticState state) {
    switch (state) {
        case StorageDiagnosticState::Queued: return "queued";
        case StorageDiagnosticState::Writing: return "writing";
        case StorageDiagnosticState::Complete: return "complete";
        case StorageDiagnosticState::Failed: return "failed";
        case StorageDiagnosticState::Idle:
        default: return "idle";
    }
}

void StorageDiagnosticJob::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutexStatic(&lock_storage_);
    if (!payload_) {
        payload_ = static_cast<uint8_t *>(
            Memory::alloc_large(PAYLOAD_CAPACITY, false));
    }

    if (!lock(20)) return;
    status_.available = lock_ && payload_;
    if (!status_.available) {
        status_.state = StorageDiagnosticState::Failed;
        copy_cstr(status_.error, sizeof(status_.error), "allocation_failed");
    }
    unlock();
}

bool StorageDiagnosticJob::lock(uint32_t timeout_ms) const {
    return lock_ &&
           xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageDiagnosticJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

bool StorageDiagnosticJob::request_append(const char *path,
                                           const uint8_t *data,
                                           size_t len) {
    if (!path || path[0] != '/' || !data || len == 0 ||
        len > PAYLOAD_CAPACITY ||
        strlen(path) >= AC_STORAGE_WRITE_PATH_MAX) {
        return false;
    }
    if (!lock(20)) return false;

    const bool busy = status_.state == StorageDiagnosticState::Queued ||
                      status_.state == StorageDiagnosticState::Writing;
    if (!status_.available || busy) {
        unlock();
        return false;
    }

    copy_cstr(status_.path, sizeof(status_.path), path);
    memcpy(payload_, data, len);
    payload_len_ = len;
    status_.bytes = len;
    status_.error[0] = '\0';
    status_.state = StorageDiagnosticState::Queued;
    unlock();
    return true;
}

StorageDiagnosticStatus StorageDiagnosticJob::status() const {
    StorageDiagnosticStatus out;
    if (!lock(20)) return out;
    out = status_;
    unlock();
    return out;
}

JobStep StorageDiagnosticJob::step() {
    if (!lock(20)) return JobStep::Waiting;
    if (status_.state != StorageDiagnosticState::Queued) {
        unlock();
        return JobStep::Idle;
    }

    char path[AC_STORAGE_WRITE_PATH_MAX] = {};
    uint8_t payload[PAYLOAD_CAPACITY] = {};
    const size_t payload_len = payload_len_;
    copy_cstr(path, sizeof(path), status_.path);
    memcpy(payload, payload_, payload_len);
    status_.state = StorageDiagnosticState::Writing;
    unlock();

    size_t written = 0;
    bool opened = false;
    {
        Storage::Guard guard;
        File file = Storage::open(path, "a");
        opened = static_cast<bool>(file);
        if (opened) {
            written = file.write(payload, payload_len);
            file.close();
        }
    }

    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) {
        return JobStep::Idle;
    }
    status_.bytes = written;
    if (!opened) {
        status_.state = StorageDiagnosticState::Failed;
        copy_cstr(status_.error, sizeof(status_.error), "open_failed");
    } else if (written != payload_len) {
        status_.state = StorageDiagnosticState::Failed;
        copy_cstr(status_.error, sizeof(status_.error), "short_write");
    } else {
        status_.state = StorageDiagnosticState::Complete;
        status_.error[0] = '\0';
    }
    unlock();
    return JobStep::Idle;
}

}  // namespace aircannect
