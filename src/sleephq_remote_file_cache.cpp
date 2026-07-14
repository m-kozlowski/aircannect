#include "sleephq_remote_file_cache.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
#include "memory_manager.h"
#endif

#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t INITIAL_ENTRY_CAPACITY = 32;
static constexpr size_t INITIAL_BUCKET_COUNT = 64;

void *alloc_zeroed(size_t count, size_t size) {
#if defined(ARDUINO)
    return Memory::calloc_large(count, size, false);
#else
    return calloc(count, size);
#endif
}

void free_large(void *ptr) {
#if defined(ARDUINO)
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

uint64_t fnv1a_append(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t fnv1a_append_string(uint64_t hash, const char *value) {
    const char *text = value ? value : "";
    hash = fnv1a_append(hash, text, strlen(text));
    const uint8_t separator = 0;
    return fnv1a_append(hash, &separator, sizeof(separator));
}

}  // namespace

SleepHqRemoteFileCache::~SleepHqRemoteFileCache() {
    clear();
}

uint64_t SleepHqRemoteFileCache::hash_key(const char *name,
                                           const char *path,
                                           const char *content_hash,
                                           uint64_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = fnv1a_append(hash, &size, sizeof(size));
    hash = fnv1a_append_string(hash, name);
    hash = fnv1a_append_string(hash, path);
    return fnv1a_append_string(hash, content_hash);
}

bool SleepHqRemoteFileCache::matches(const Entry &entry,
                                     uint64_t key_hash,
                                     const char *name,
                                     const char *path,
                                     const char *content_hash,
                                     uint64_t size) {
    return entry.key_hash == key_hash && entry.size == size &&
           strcmp(entry.name, name) == 0 &&
           strcmp(entry.path, path) == 0 &&
           strcmp(entry.content_hash, content_hash) == 0;
}

bool SleepHqRemoteFileCache::reserve_entries(size_t needed) {
    if (needed <= capacity_) return true;

    size_t next = capacity_ == 0 ? INITIAL_ENTRY_CAPACITY : capacity_ * 2;
    while (next < needed) next *= 2;

    Entry *entries = static_cast<Entry *>(alloc_zeroed(next, sizeof(Entry)));
    if (!entries) return false;
    for (size_t i = 0; i < next; ++i) new (&entries[i]) Entry();
    for (size_t i = 0; i < count_; ++i) entries[i] = entries_[i];

    if (entries_) free_large(entries_);
    entries_ = entries;
    capacity_ = next;
    return true;
}

bool SleepHqRemoteFileCache::reserve_buckets(size_t needed_entries) {
    size_t next = bucket_count_ == 0 ? INITIAL_BUCKET_COUNT : bucket_count_;
    while (needed_entries * 10 >= next * 7) next *= 2;
    if (next == bucket_count_) return true;

    uint32_t *buckets = static_cast<uint32_t *>(
        alloc_zeroed(next, sizeof(uint32_t)));
    if (!buckets) return false;

    if (buckets_) free_large(buckets_);
    buckets_ = buckets;
    bucket_count_ = next;
    for (size_t i = 0; i < count_; ++i) insert_bucket(i);
    return true;
}

void SleepHqRemoteFileCache::insert_bucket(size_t entry_index) {
    const Entry &entry = entries_[entry_index];
    size_t bucket = static_cast<size_t>(entry.key_hash) & (bucket_count_ - 1);
    while (buckets_[bucket] != 0) {
        bucket = (bucket + 1) & (bucket_count_ - 1);
    }
    buckets_[bucket] = static_cast<uint32_t>(entry_index + 1);
}

size_t SleepHqRemoteFileCache::find_bucket(uint64_t key_hash,
                                           const char *name,
                                           const char *path,
                                           const char *content_hash,
                                           uint64_t size) const {
    if (!buckets_ || bucket_count_ == 0) return SIZE_MAX;

    size_t bucket = static_cast<size_t>(key_hash) & (bucket_count_ - 1);
    for (size_t probed = 0; probed < bucket_count_; ++probed) {
        const uint32_t stored = buckets_[bucket];
        if (stored == 0) return bucket;

        const size_t entry_index = static_cast<size_t>(stored - 1);
        if (entry_index < count_ &&
            matches(entries_[entry_index], key_hash, name, path,
                    content_hash, size)) {
            return bucket;
        }
        bucket = (bucket + 1) & (bucket_count_ - 1);
    }
    return SIZE_MAX;
}

bool SleepHqRemoteFileCache::add(const SleepHqRemoteFile &file) {
    if (!file.name[0] || !file.path[0] || !file.content_hash[0]) return true;
    if (!reserve_buckets(count_ + 1)) return false;

    const uint64_t key_hash =
        hash_key(file.name, file.path, file.content_hash, file.size);
    size_t bucket = find_bucket(key_hash, file.name, file.path,
                                file.content_hash, file.size);
    if (bucket == SIZE_MAX) return false;
    if (buckets_[bucket] != 0) return true;
    if (!reserve_entries(count_ + 1)) return false;

    Entry &entry = entries_[count_];
    entry.key_hash = key_hash;
    entry.size = file.size;
    copy_cstr(entry.name, sizeof(entry.name), file.name);
    copy_cstr(entry.path, sizeof(entry.path), file.path);
    copy_cstr(entry.content_hash, sizeof(entry.content_hash),
              file.content_hash);
    buckets_[bucket] = static_cast<uint32_t>(++count_);
    return true;
}

bool SleepHqRemoteFileCache::contains(const char *name,
                                      const char *path,
                                      const char *content_hash,
                                      uint64_t size) const {
    if (!name || !name[0] || !path || !path[0] ||
        !content_hash || !content_hash[0]) {
        return false;
    }
    const uint64_t key_hash = hash_key(name, path, content_hash, size);
    const size_t bucket = find_bucket(key_hash, name, path,
                                      content_hash, size);
    return bucket != SIZE_MAX && buckets_[bucket] != 0;
}

void SleepHqRemoteFileCache::clear() {
    if (entries_) {
        for (size_t i = 0; i < capacity_; ++i) entries_[i].~Entry();
        free_large(entries_);
    }
    if (buckets_) free_large(buckets_);

    entries_ = nullptr;
    count_ = 0;
    capacity_ = 0;
    buckets_ = nullptr;
    bucket_count_ = 0;
}

}  // namespace aircannect
