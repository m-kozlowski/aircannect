#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "operation_outcome.h"
#include "storage_path.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_SCAN_ROOT_MAX = 8;

struct StorageScanRoot {
    const char *path = nullptr;
    bool recursive = false;
};

struct StorageScanCommand {
    // request_scan() copies every root before returning.
    const StorageScanRoot *roots = nullptr;
    size_t root_count = 0;
    bool include_directories = false;
    uint32_t generation = 0;

    bool valid() const {
        return roots && root_count > 0 &&
               root_count <= AC_STORAGE_SCAN_ROOT_MAX && generation != 0;
    }
};

struct StorageScanEntryView {
    const char *path = nullptr;
    bool directory = false;
    uint8_t root_index = 0;
    uint64_t size = 0;
    uint64_t modified = 0;
};

class StorageScanSnapshot {
public:
    ~StorageScanSnapshot();

    size_t size() const { return entry_count_; }
    uint32_t generation() const { return generation_; }
    bool entry(size_t index, StorageScanEntryView &out) const;

private:
    friend class StorageScanService;

    struct Entry {
        uint64_t size = 0;
        uint64_t modified = 0;
        uint32_t path_offset = 0;
        bool directory = false;
        uint8_t root_index = 0;
    };

    Entry *entries_ = nullptr;
    size_t entry_count_ = 0;
    size_t entry_capacity_ = 0;
    char *paths_ = nullptr;
    size_t paths_length_ = 0;
    size_t paths_capacity_ = 0;
    uint32_t generation_ = 0;
};

struct StorageScanCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    std::shared_ptr<const StorageScanSnapshot> snapshot;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class StorageScanPort {
public:
    virtual ~StorageScanPort() = default;

    virtual OperationSubmission request_scan(
        const StorageScanCommand &command) = 0;
    virtual bool abandon(OperationTicket ticket) = 0;
    virtual bool take_completion(
        OperationTicket ticket,
        StorageScanCompletion &completion) = 0;
};

}  // namespace aircannect
