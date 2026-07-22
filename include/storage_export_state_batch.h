#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "storage_path.h"

namespace aircannect {

class StorageExportInventoryView;

class StorageExportStateBatch {
public:
    void clear();
    bool add(const char *state_path,
             const char *local_path,
             uint64_t size,
             uint64_t mtime);

    bool empty() const { return count_ == 0; }
    const char *first_state_path() const;
    std::shared_ptr<const LargeByteBuffer> build_file(
        const StorageExportInventoryView &inventory,
        const char *state_path) const;
    void erase_state_path(const char *state_path);

private:
    struct Entry {
        char state_path[AC_STORAGE_PATH_MAX] = {};
        char local_path[AC_STORAGE_PATH_MAX] = {};
        uint64_t size = 0;
        uint64_t mtime = 0;
    };

    bool reserve(size_t needed);
    Entry *entries();
    const Entry *entries() const;

    std::unique_ptr<LargeByteBuffer> entries_buffer_;
    size_t count_ = 0;
    size_t capacity_ = 0;
};

}  // namespace aircannect
