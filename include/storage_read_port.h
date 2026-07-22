#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "operation_outcome.h"

namespace aircannect {

enum class StorageReadLane : uint8_t {
    Foreground,
    Report,
    Export,
    Maintenance,
};

struct StorageReadCommand {
    std::string path;
    uint64_t offset = 0;
    size_t length = 0;
    StorageReadLane lane = StorageReadLane::Report;
    uint32_t generation = 0;

    bool valid() const {
        return !path.empty() && path.front() == '/' && length != 0 &&
               generation != 0;
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
};

class StorageReadPort {
public:
    virtual ~StorageReadPort() = default;

    virtual OperationSubmission request_read(
        const StorageReadCommand &command) = 0;
    virtual bool cancel(OperationTicket ticket) = 0;
    virtual bool next_completion(StorageReadCompletion &completion) = 0;

    virtual size_t read_prepared(StoragePreparedRead prepared,
                                 size_t offset,
                                 uint8_t *buffer,
                                 size_t capacity) const = 0;
    virtual void release_prepared(StoragePreparedRead prepared) = 0;
};

}  // namespace aircannect
