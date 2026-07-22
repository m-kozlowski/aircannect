#pragma once

#include <memory>
#include <stdint.h>
#include <string>

#include "large_byte_buffer.h"
#include "operation_outcome.h"
#include "storage_path.h"

namespace aircannect {

enum class StorageAtomicWriteLane : uint8_t {
    Foreground,
    Maintenance,
};

struct StorageAtomicWriteCommand {
    std::string path;
    std::shared_ptr<const LargeByteBuffer> bytes;
    std::string staged_path;
    uint64_t staged_size = 0;
    uint64_t free_reserve_bytes = 0;
    StorageAtomicWriteLane lane = StorageAtomicWriteLane::Maintenance;
    uint32_t generation = 0;
    bool replace_existing = true;

    bool valid() const {
        const bool memory_source = bytes && bytes->size() != 0 &&
                                   staged_path.empty() && staged_size == 0;
        const bool staged_source = !bytes && !staged_path.empty() &&
                                   storage_user_path_valid(
                                       staged_path.c_str()) &&
                                   staged_path != "/";
        return generation != 0 && storage_user_path_valid(path.c_str()) &&
               path != "/" && (memory_source || staged_source) &&
               staged_path != path;
    }
};

struct StorageAtomicWriteCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    uint64_t bytes_written = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// The storage owner keeps the previous file until the replacement and its
// recovery record are durable. A completed operation exposes the whole new
// file; an interrupted operation is recovered before other storage work.
class StorageAtomicWritePort {
public:
    virtual ~StorageAtomicWritePort() = default;

    virtual OperationSubmission request_write(const StorageAtomicWriteCommand &command) = 0;
    virtual bool abandon(OperationTicket ticket) = 0;
    virtual bool take_completion(OperationTicket ticket, StorageAtomicWriteCompletion &completion) = 0;
};

}  // namespace aircannect
