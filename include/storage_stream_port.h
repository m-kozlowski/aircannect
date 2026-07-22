#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "storage_path.h"

namespace aircannect {

struct StorageByteStream;

enum class StorageStreamLane : uint8_t {
    Foreground,
    Export,
};

enum class StorageStreamState : uint8_t {
    Preparing,
    Ready,
    Error,
    Cancelled,
};

enum class StorageStreamReadState : uint8_t {
    Data,
    Retry,
    End,
    Error,
};

struct StorageStreamCommand {
    std::string path;
    StorageStreamLane lane = StorageStreamLane::Export;
    uint64_t expected_size = 0;
    uint64_t expected_modified = 0;
    bool verify_snapshot = false;

    bool valid() const {
        return !path.empty() && path.front() == '/';
    }
};

struct StorageStreamStatus {
    StorageStreamState state = StorageStreamState::Preparing;
    uint64_t size = 0;
    uint64_t modified = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

struct StorageStreamRead {
    StorageStreamReadState state = StorageStreamReadState::End;
    size_t bytes = 0;
};

class StorageStreamPort {
public:
    virtual ~StorageStreamPort() = default;

    virtual bool request_stream(
        const StorageStreamCommand &command,
        std::shared_ptr<StorageByteStream> &stream_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) = 0;
    virtual bool status(const StorageByteStream &stream,
                        StorageStreamStatus &status_out) const = 0;
    virtual bool attach(StorageByteStream &stream) = 0;
    virtual StorageStreamRead read(StorageByteStream &stream,
                                   uint8_t *buffer,
                                   size_t max_length,
                                   size_t offset) = 0;
    virtual void finish(StorageByteStream &stream, bool complete) = 0;
};

}  // namespace aircannect
