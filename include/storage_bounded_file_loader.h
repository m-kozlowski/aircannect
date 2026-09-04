#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "storage_read_port.h"

namespace aircannect {

enum class StorageBoundedFileLoadState : uint8_t {
    Idle,
    Submitting,
    Waiting,
    Copying,
    Ready,
    Missing,
    Failed,
    Cancelled,
};

struct StorageBoundedFileLoadStatus {
    StorageBoundedFileLoadState state = StorageBoundedFileLoadState::Idle;
    StorageReadLane lane = StorageReadLane::Maintenance;
    size_t bytes_loaded = 0;
    uint64_t modified = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const;
    bool terminal() const;
};

class StorageBoundedFileLoader {
public:
    StorageBoundedFileLoader() = default;
    ~StorageBoundedFileLoader();

    StorageBoundedFileLoader(const StorageBoundedFileLoader &) = delete;
    StorageBoundedFileLoader &operator=(
        const StorageBoundedFileLoader &) = delete;

    void begin(StorageReadPort &read_port);
    OperationAdmission start(const char *path,
                             size_t maximum_size,
                             uint32_t generation,
                             StorageReadLane lane);
    bool poll();
    void cancel();
    void reset();

    const StorageBoundedFileLoadStatus &status() const { return status_; }
    std::shared_ptr<const LargeByteBuffer> take_completed();

private:
    bool submit();
    bool finish_read();
    bool copy();
    void finish(StorageBoundedFileLoadState state, const char *error);
    void release_prepared();
    void clear_operation();

    StorageReadPort *read_port_ = nullptr;
    std::unique_ptr<LargeByteBuffer> buffer_;
    std::shared_ptr<const LargeByteBuffer> completed_;
    OperationTicket ticket_;
    StoragePreparedRead prepared_;
    uint32_t generation_ = 0;
    size_t maximum_size_ = 0;
    size_t copied_ = 0;
    char path_[AC_STORAGE_PATH_MAX] = {};
    StorageBoundedFileLoadStatus status_;
};

}  // namespace aircannect
