#include "storage_export_state_batch.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "storage_export_inventory.h"
#include "storage_export_plan.h"
#include "string_util.h"

namespace aircannect {
namespace {

bool format_state_line(char *out,
                       size_t out_size,
                       const char *path,
                       uint64_t size,
                       uint64_t mtime,
                       size_t &length_out) {
    const int written = snprintf(
        out,
        out_size,
        "%llu\t%llu\t%s\n",
        static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(mtime),
        path ? path : "");
    if (written <= 0 || static_cast<size_t>(written) >= out_size) return false;

    length_out = static_cast<size_t>(written);
    return true;
}

bool entry_uses_state_path(const StorageExportInventoryEntryView &entry,
                           const char *state_dir,
                           const char *state_path) {
    char expected[AC_STORAGE_PATH_MAX] = {};
    return entry.path && entry.local_state_complete &&
           storage_export_build_state_path(state_dir,
                                           entry.path,
                                           expected,
                                           sizeof(expected)) &&
           strcmp(expected, state_path) == 0;
}

bool add_length(size_t &total, size_t length) {
    if (length > SIZE_MAX - total) return false;

    total += length;
    return true;
}

}  // namespace

StorageExportStateBatch::Entry *StorageExportStateBatch::entries() {
    return entries_buffer_
               ? reinterpret_cast<Entry *>(entries_buffer_->data())
               : nullptr;
}

const StorageExportStateBatch::Entry *StorageExportStateBatch::entries() const {
    return entries_buffer_
               ? reinterpret_cast<const Entry *>(entries_buffer_->data())
               : nullptr;
}

void StorageExportStateBatch::clear() {
    entries_buffer_.reset();
    count_ = 0;
    capacity_ = 0;
}

bool StorageExportStateBatch::reserve(size_t needed) {
    if (needed <= capacity_) return true;

    size_t next = capacity_ == 0 ? 8 : capacity_;
    while (next < needed) {
        if (next > SIZE_MAX / 2) return false;
        next *= 2;
    }
    if (next > SIZE_MAX / sizeof(Entry)) return false;

    std::unique_ptr<LargeByteBuffer> buffer =
        LargeByteBuffer::allocate(next * sizeof(Entry));
    if (!buffer) return false;

    memset(buffer->data(), 0, buffer->size());
    if (count_ != 0) {
        memcpy(buffer->data(), entries(), count_ * sizeof(Entry));
    }

    entries_buffer_ = std::move(buffer);
    capacity_ = next;
    return true;
}

bool StorageExportStateBatch::add(const char *state_path,
                                  const char *local_path,
                                  uint64_t size,
                                  uint64_t mtime) {
    if (!state_path || !state_path[0] || !local_path || !local_path[0]) {
        return false;
    }

    for (size_t i = 0; i < count_; ++i) {
        Entry &entry = entries()[i];
        if (strcmp(entry.state_path, state_path) != 0 ||
            strcmp(entry.local_path, local_path) != 0) {
            continue;
        }

        entry.size = size;
        entry.mtime = mtime;
        return true;
    }
    if (!reserve(count_ + 1)) return false;

    Entry &entry = entries()[count_++];
    copy_cstr(entry.state_path, sizeof(entry.state_path), state_path);
    copy_cstr(entry.local_path, sizeof(entry.local_path), local_path);
    entry.size = size;
    entry.mtime = mtime;
    return true;
}

const char *StorageExportStateBatch::first_state_path() const {
    return count_ != 0 ? entries()[0].state_path : nullptr;
}

std::shared_ptr<const LargeByteBuffer> StorageExportStateBatch::build_file(
    const StorageExportInventoryView &inventory,
    const char *state_path) const {
    if (!state_path || !state_path[0]) return {};

    char line[AC_STORAGE_PATH_MAX + 64] = {};
    size_t total = 0;
    size_t line_length = 0;
    for (size_t i = 0; i < inventory.source_size(); ++i) {
        StorageExportInventoryEntryView entry;
        if (!inventory.entry(i, entry)) continue;
        if (!entry_uses_state_path(entry, inventory.state_dir(), state_path)) {
            continue;
        }
        if (!format_state_line(line,
                               sizeof(line),
                               entry.path,
                               entry.info.size,
                               entry.info.mtime,
                               line_length) ||
            !add_length(total, line_length)) {
            return {};
        }
    }
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries()[i];
        if (strcmp(entry.state_path, state_path) != 0) continue;

        StorageExportInventoryEntryView existing;
        if (inventory.find_file(entry.local_path, existing) &&
            existing.local_state_complete &&
            existing.info.size == entry.size &&
            existing.info.mtime == entry.mtime) {
            continue;
        }
        if (!format_state_line(line,
                               sizeof(line),
                               entry.local_path,
                               entry.size,
                               entry.mtime,
                               line_length) ||
            !add_length(total, line_length)) {
            return {};
        }
    }
    if (total == 0) return {};

    std::unique_ptr<LargeByteBuffer> bytes = LargeByteBuffer::allocate(total);
    if (!bytes) return {};

    size_t offset = 0;
    for (size_t i = 0; i < inventory.source_size(); ++i) {
        StorageExportInventoryEntryView entry;
        if (!inventory.entry(i, entry)) continue;
        if (!entry_uses_state_path(entry, inventory.state_dir(), state_path)) {
            continue;
        }
        if (!format_state_line(line,
                               sizeof(line),
                               entry.path,
                               entry.info.size,
                               entry.info.mtime,
                               line_length)) {
            return {};
        }

        memcpy(bytes->data() + offset, line, line_length);
        offset += line_length;
    }
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries()[i];
        if (strcmp(entry.state_path, state_path) != 0) continue;

        StorageExportInventoryEntryView existing;
        if (inventory.find_file(entry.local_path, existing) &&
            existing.local_state_complete &&
            existing.info.size == entry.size &&
            existing.info.mtime == entry.mtime) {
            continue;
        }
        if (!format_state_line(line,
                               sizeof(line),
                               entry.local_path,
                               entry.size,
                               entry.mtime,
                               line_length)) {
            return {};
        }

        memcpy(bytes->data() + offset, line, line_length);
        offset += line_length;
    }
    if (offset != total) return {};

    return LargeByteBuffer::freeze(std::move(bytes));
}

void StorageExportStateBatch::erase_state_path(const char *state_path) {
    if (!state_path) return;

    size_t out = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(entries()[i].state_path, state_path) == 0) continue;
        if (out != i) entries()[out] = entries()[i];
        out++;
    }
    count_ = out;
}

}  // namespace aircannect
