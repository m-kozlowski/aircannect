#pragma once

#include <memory>
#include <stdint.h>
#include <string>

#include "operation_outcome.h"
#include "storage_path.h"
#include "storage_write_payload.h"

namespace aircannect {

enum class StorageAtomicWriteLane : uint8_t {
    Foreground,
    Maintenance,
};

struct StorageAtomicWriteCommand : StorageWritePayload {
    std::string path;
    std::string staged_path;
    uint64_t staged_size = 0;
    uint64_t free_reserve_bytes = 0;
    StorageAtomicWriteLane lane = StorageAtomicWriteLane::Maintenance;
    uint32_t generation = 0;
    bool replace_existing = true;

    bool valid() const {
        uint64_t source_size = 0;
        return valid(source_size);
    }

    // Return the accepted length so the storage job need not rescan the chain.
    bool valid(uint64_t &source_size) const {
        source_size = 0;
        const bool memory_source = (bytes || buffers) &&
                                   staged_path.empty() && staged_size == 0;
        const bool staged_source = !bytes && !buffers &&
                                   !staged_path.empty() &&
                                   storage_user_path_valid(
                                       staged_path.c_str()) &&
                                   staged_path != "/";

        if (generation == 0 || !storage_user_path_valid(path.c_str()) ||
            path == "/" || (!memory_source && !staged_source) ||
            staged_path == path) {
            return false;
        }

        source_size = memory_source ? size() : staged_size;
        return staged_source || source_size != 0;
    }
};

struct StorageAtomicWriteCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    uint64_t bytes_written = 0;
    uint64_t modified = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// The storage owner keeps the previous file until the replacement and its
// recovery record are durable. A completed operation exposes the whole new
// file; an interrupted operation is recovered before other storage work.
class StorageAtomicWritePort {
public:
    virtual ~StorageAtomicWritePort() = default;

    // Directory migration must wait for recovery of old absolute target paths.
    virtual bool recovery_complete() const = 0;

    // Admission and completion never wait for filesystem I/O. Busy leaves
    // submission with the caller; false completion leaves the ticket pending.
    virtual OperationSubmission request_write(const StorageAtomicWriteCommand &command) = 0;
    // Acceptance transfers cancellation to the storage owner. The caller may
    // release the ticket without waiting for in-progress filesystem I/O.
    virtual bool abandon(OperationTicket ticket) = 0;
    virtual bool take_completion(OperationTicket ticket, StorageAtomicWriteCompletion &completion) = 0;
};

}  // namespace aircannect
