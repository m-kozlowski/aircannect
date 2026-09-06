#pragma once

#include "storage_atomic_write_port.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_RANGE_WRITE_MAX_BYTES = 256 * 1024;
static constexpr size_t AC_STORAGE_RANGE_WRITE_STEP_BYTES = 16 * 1024;

struct StorageRangeWriteCommand {
    std::string path;
    std::shared_ptr<const LargeByteBuffer> bytes;
    uint64_t offset = 0;
    bool truncate = false;
    // Reuse hint only; successful completion still means the bytes are flushed.
    bool retain_handle = false;
    uint32_t generation = 0;
    StorageAtomicWriteLane lane = StorageAtomicWriteLane::Maintenance;

    bool valid() const {
        return generation != 0 && bytes && bytes->size() != 0 &&
               bytes->size() <= AC_STORAGE_RANGE_WRITE_MAX_BYTES &&
               offset <= UINT32_MAX - bytes->size() &&
               (!truncate || offset == 0) &&
               (lane == StorageAtomicWriteLane::Foreground ||
                lane == StorageAtomicWriteLane::Maintenance) &&
               path.find('\0') == std::string::npos &&
               storage_user_path_valid(path.c_str()) && path != "/";
    }
};

struct StorageRangeWriteCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    uint64_t bytes_written = 0;
    // Flushed file mtime on success, or zero when unavailable.
    uint64_t modified = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// Explicitly non-atomic: failed or abandoned writes may leave a partial range
// (or a truncated file). No rollback, recovery record, or publication occurs.
// Offset-zero writes create missing parents, one directory per owner turn.
// Missing files can only be created at offset zero;
// offsets beyond EOF fail. Existing bytes outside the range are preserved
// unless truncate is requested. Callers serialize mutations of the same path.
class StorageRangeWritePort {
public:
    virtual ~StorageRangeWritePort() = default;

    // One request or unconsumed completion occupies the port. Accepted bytes
    // remain shared and immutable until completion or owner-side abandonment.
    virtual OperationSubmission request_write(
        const StorageRangeWriteCommand &command) = 0;

    // Acceptance queues owner-side cancellation without waiting for file I/O.
    // The caller can release the ticket; no completion is delivered afterwards.
    virtual bool abandon(OperationTicket ticket) = 0;

    // Advisory reuse ends at the producer's build boundary. This queues
    // closure on the storage task, including handles from completed writes.
    virtual void release_handles() = 0;

    virtual bool take_completion(
        OperationTicket ticket, StorageRangeWriteCompletion &completion) = 0;
};

}  // namespace aircannect
