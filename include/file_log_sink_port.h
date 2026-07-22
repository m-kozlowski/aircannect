#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct FileLogSinkStatus {
    bool available = false;
    bool enabled = false;
    bool open = false;
    size_t queue_capacity = 0;
    size_t queued = 0;
    uint64_t bytes = 0;
    uint32_t written = 0;
    uint32_t drops = 0;
    uint32_t errors = 0;
};

class FileLogSinkPort {
public:
    virtual ~FileLogSinkPort() = default;

    virtual bool configure(bool enabled) = 0;
    virtual bool enqueue(const char *line, size_t length) = 0;
    virtual FileLogSinkStatus status() const = 0;
};

}  // namespace aircannect
