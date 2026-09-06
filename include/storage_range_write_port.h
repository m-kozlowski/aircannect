#pragma once

#include "storage_atomic_write_port.h"
#include "large_scratch_array.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_RANGE_WRITE_MAX_BYTES = 256 * 1024;
static constexpr size_t AC_STORAGE_RANGE_WRITE_STEP_BYTES = 16 * 1024;

using StorageWriteBuffers =
    LargeScratchArray<std::shared_ptr<const LargeByteBuffer>>;

struct StorageRangeWriteCommand {
    std::string path;
    std::shared_ptr<const LargeByteBuffer> bytes;
    // Optional buffers following bytes, in order. No copy into a flat payload.
    std::shared_ptr<const StorageWriteBuffers> buffers;
    uint64_t offset = 0;
    bool truncate = false;
    // Retained writes complete after write(), and close at the build boundary.
    bool retain_handle = false;
    bool finish = false;
    uint32_t generation = 0;
    StorageAtomicWriteLane lane = StorageAtomicWriteLane::Maintenance;

    size_t size() const {
        size_t total = bytes ? bytes->size() : 0;
        if (buffers) {
            for (size_t i = 0; i < buffers->size(); ++i) {
                const auto &part = buffers->data()[i];
                if (!part || part->size() > AC_STORAGE_RANGE_WRITE_MAX_BYTES ||
                    total > AC_STORAGE_RANGE_WRITE_MAX_BYTES - part->size()) {
                    return 0;
                }
                total += part->size();
            }
        }
        return total;
    }

    const uint8_t *span(size_t position, size_t &length) const {
        const size_t prefix = bytes ? bytes->size() : 0;
        if (position < prefix) {
            length = prefix - position;
            return bytes->data() + position;
        }
        position -= prefix;
        if (buffers) {
            for (size_t i = 0; i < buffers->size(); ++i) {
                const auto &part = buffers->data()[i];
                if (position < part->size()) {
                    length = part->size() - position;
                    return part->data() + position;
                }
                position -= part->size();
            }
        }
        length = 0;
        return nullptr;
    }

    bool valid() const {
        if (!generation ||
            (lane != StorageAtomicWriteLane::Foreground &&
             lane != StorageAtomicWriteLane::Maintenance)) return false;

        if (finish) return path.empty() && !bytes && !buffers &&
                           !offset && !truncate && !retain_handle;

        const size_t length = size();
        return length != 0 && length <= AC_STORAGE_RANGE_WRITE_MAX_BYTES &&
               offset <= UINT32_MAX - length &&
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
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// Explicitly non-atomic: failed or abandoned writes may leave a partial range
// (or a truncated file). No rollback, recovery record, or publication occurs.
// Offset-zero writes create missing parents, one directory per owner turn.
// Missing files can only be created at offset zero;
// offsets beyond EOF fail. Existing bytes outside the range are preserved
// unless truncate is requested. Callers serialize mutations of the same path.
// A finish command closes retained handles and reports delayed close errors.
// Submit it before publishing files that reference the written ranges.
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
