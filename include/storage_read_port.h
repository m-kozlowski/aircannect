#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "operation_outcome.h"
#include "storage_path.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_PREPARED_READ_MAX_BYTES = 512 * 1024;

enum class StorageReadLane : uint8_t {
    Foreground,
    Report,
    Export,
    Maintenance,
};

enum class StorageReadMode : uint8_t {
    Range,
    TailLines,
};

struct StorageReadCommand {
    std::string path;
    StorageReadMode mode = StorageReadMode::Range;
    uint64_t offset = 0;
    size_t length = 0;
    size_t tail_lines = 0;
    StorageReadLane lane = StorageReadLane::Report;
    uint32_t generation = 0;

    bool valid() const {
        if (path.empty() || path.front() != '/' || length == 0 ||
            generation == 0) {
            return false;
        }
        return mode == StorageReadMode::Range ||
               (mode == StorageReadMode::TailLines && tail_lines != 0);
    }
};

struct StoragePreparedRead {
    uint32_t id = 0;
    size_t length = 0;

    bool valid() const { return id != 0; }
};

struct StorageReadCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    StoragePreparedRead prepared;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class StorageReadPort {
public:
    virtual ~StorageReadPort() = default;

    virtual OperationSubmission request_read(
        const StorageReadCommand &command) = 0;
    virtual bool cancel(OperationTicket ticket) = 0;
    virtual bool abandon(OperationTicket ticket) = 0;
    virtual bool take_completion(
        OperationTicket ticket,
        StorageReadCompletion &completion) = 0;

    virtual size_t read_prepared(StoragePreparedRead prepared,
                                 size_t offset,
                                 uint8_t *buffer,
                                 size_t capacity) const = 0;
    virtual void release_prepared(StoragePreparedRead prepared) = 0;
};

}  // namespace aircannect
