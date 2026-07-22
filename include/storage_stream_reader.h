#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "background_operation_control.h"
#include "storage_stream_port.h"

namespace aircannect {

class StorageStreamReader {
public:
    StorageStreamReader() = default;
    ~StorageStreamReader();
    StorageStreamReader(const StorageStreamReader &) = delete;
    StorageStreamReader &operator=(const StorageStreamReader &) = delete;
    StorageStreamReader(StorageStreamReader &&other) noexcept;
    StorageStreamReader &operator=(StorageStreamReader &&other) noexcept;

    void configure(StorageStreamPort &port,
                   const char *path,
                   uint64_t expected_size,
                   uint64_t expected_modified);
    void configure(StorageStreamPort &port,
                   const StorageStreamCommand &command);
    bool open(const BackgroundOperationControl &operation,
              char *error_out,
              size_t error_out_size);
    bool restart(const BackgroundOperationControl &operation,
                 char *error_out,
                 size_t error_out_size);
    bool read_exact(uint8_t *buffer,
                    size_t length,
                    const BackgroundOperationControl &operation,
                    char *error_out,
                    size_t error_out_size);
    void close(bool complete);

    bool open() const { return stream_ != nullptr; }
    uint64_t offset() const { return offset_; }
    uint64_t size() const { return size_; }
    uint64_t modified() const { return modified_; }

private:
    void move_from(StorageStreamReader &other);
    bool wait_for_request_slot(
        const BackgroundOperationControl &operation,
        char *error_out,
        size_t error_out_size);

    StorageStreamPort *port_ = nullptr;
    StorageStreamCommand command_;
    std::shared_ptr<StorageByteStream> stream_;
    uint64_t offset_ = 0;
    uint64_t size_ = 0;
    uint64_t modified_ = 0;
};

}  // namespace aircannect
