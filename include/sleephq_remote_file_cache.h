#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sleephq_protocol.h"

namespace aircannect {

class SleepHqRemoteFileCache {
public:
    SleepHqRemoteFileCache() = default;
    ~SleepHqRemoteFileCache();

    SleepHqRemoteFileCache(const SleepHqRemoteFileCache &) = delete;
    SleepHqRemoteFileCache &operator=(const SleepHqRemoteFileCache &) = delete;

    bool add(const SleepHqRemoteFile &file);
    bool contains(const char *name, const char *path,
                  const char *content_hash, uint64_t size) const;
    void clear();

    size_t size() const { return count_; }

private:
    struct Entry {
        uint64_t key_hash = 0;
        uint64_t size = 0;
        char name[AC_STORAGE_NAME_MAX] = {};
        char path[AC_STORAGE_PATH_MAX] = {};
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
    };

    static uint64_t hash_key(const char *name, const char *path,
                             const char *content_hash, uint64_t size);
    static bool matches(const Entry &entry, uint64_t key_hash,
                        const char *name, const char *path,
                        const char *content_hash, uint64_t size);

    bool reserve_entries(size_t needed);
    bool reserve_buckets(size_t needed_entries);
    void insert_bucket(size_t entry_index);
    size_t find_bucket(uint64_t key_hash, const char *name,
                       const char *path, const char *content_hash,
                       uint64_t size) const;

    Entry *entries_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;

    uint32_t *buckets_ = nullptr;
    size_t bucket_count_ = 0;
};

}  // namespace aircannect
