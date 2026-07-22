#include "resmed_firmware_catalog.h"

#include <algorithm>
#include <ctype.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

bool ends_with_case_insensitive(const char *value, const char *suffix) {
    if (!value || !suffix) return false;

    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    if (value_length < suffix_length) return false;

    const char *tail = value + value_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        if (tolower(static_cast<unsigned char>(tail[i])) !=
            tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

void *allocate_catalog_memory(size_t count, size_t size) {
    if (count == 0 || size == 0) return nullptr;
#ifdef ARDUINO
    return Memory::calloc_large(count, size, false);
#else
    return calloc(count, size);
#endif
}

void free_catalog_memory(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

const char *path_filename(const char *path) {
    if (!path) return nullptr;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

}  // namespace

const char *resmed_firmware_name_hint_name(ResmedFirmwareNameHint hint) {
    switch (hint) {
        case ResmedFirmwareNameHint::Abc: return "abc";
        case ResmedFirmwareNameHint::Raw: return "raw";
        case ResmedFirmwareNameHint::Unsupported: return "unsupported";
    }
    return "unsupported";
}

ResmedFirmwareNameHint resmed_firmware_name_hint_for_filename(
    const char *filename) {
    if (ends_with_case_insensitive(filename, ".abc")) {
        return ResmedFirmwareNameHint::Abc;
    }
    if (ends_with_case_insensitive(filename, ".bin") ||
        ends_with_case_insensitive(filename, ".img")) {
        return ResmedFirmwareNameHint::Raw;
    }
    return ResmedFirmwareNameHint::Unsupported;
}

ResmedFirmwareCatalogSnapshot::~ResmedFirmwareCatalogSnapshot() {
    free_catalog_memory(entries_);
    free_catalog_memory(paths_);
}

std::shared_ptr<const ResmedFirmwareCatalogSnapshot>
ResmedFirmwareCatalogSnapshot::build(const StorageScanSnapshot &scan,
                                     uint32_t revision) {
    if (revision == 0) return {};

    size_t entry_count = 0;
    size_t path_bytes = 0;
    bool truncated = false;
    for (size_t i = 0; i < scan.size(); ++i) {
        StorageScanEntryView source;
        if (!scan.entry(i, source) || source.directory || !source.path ||
            !source.path[0]) {
            continue;
        }
        if (entry_count == MaxEntries) {
            truncated = true;
            continue;
        }

        const size_t length = strlen(source.path) + 1;
        if (length > UINT32_MAX || path_bytes > UINT32_MAX - length) {
            return {};
        }
        path_bytes += length;
        entry_count++;
    }

    std::shared_ptr<ResmedFirmwareCatalogSnapshot> result(
        new (std::nothrow) ResmedFirmwareCatalogSnapshot());
    if (!result) return {};

    if (entry_count > 0) {
        result->entries_ = static_cast<Entry *>(
            allocate_catalog_memory(entry_count, sizeof(Entry)));
        result->paths_ = static_cast<char *>(
            allocate_catalog_memory(path_bytes, sizeof(char)));
        if (!result->entries_ || !result->paths_) return {};
    }

    size_t output_index = 0;
    size_t path_offset = 0;
    for (size_t i = 0; i < scan.size() && output_index < entry_count; ++i) {
        StorageScanEntryView source;
        if (!scan.entry(i, source) || source.directory || !source.path ||
            !source.path[0]) {
            continue;
        }

        const size_t length = strlen(source.path) + 1;
        memcpy(result->paths_ + path_offset, source.path, length);

        const char *filename = path_filename(source.path);
        Entry &entry = result->entries_[output_index++];
        entry.size = source.size;
        entry.modified = source.modified;
        entry.path_offset = static_cast<uint32_t>(path_offset);
        entry.filename_offset = static_cast<uint32_t>(
            path_offset + (filename - source.path));
        entry.name_hint = resmed_firmware_name_hint_for_filename(filename);
        path_offset += length;
    }

    result->entry_count_ = output_index;
    result->paths_length_ = path_offset;
    result->revision_ = revision;
    result->truncated_ = truncated;

    if (output_index > 1) {
        Entry *entries = result->entries_;
        const char *paths = result->paths_;
        std::sort(entries, entries + output_index,
                  [paths](const Entry &left, const Entry &right) {
            if (left.modified != right.modified) {
                return left.modified > right.modified;
            }
            return strcmp(paths + left.path_offset,
                          paths + right.path_offset) < 0;
        });
    }
    return result;
}

bool ResmedFirmwareCatalogSnapshot::entry(
    size_t index,
    ResmedFirmwareEntryView &out) const {
    out = {};
    if (index >= entry_count_ || !entries_ || !paths_) return false;

    const Entry &entry = entries_[index];
    if (entry.path_offset >= paths_length_ ||
        entry.filename_offset >= paths_length_) {
        return false;
    }

    out.path = paths_ + entry.path_offset;
    out.filename = paths_ + entry.filename_offset;
    out.name_hint = entry.name_hint;
    out.size = entry.size;
    out.modified = entry.modified;
    return true;
}

bool ResmedFirmwareCatalogSnapshot::contains_file(const char *path) const {
    if (!path || !path[0]) return false;
    for (size_t i = 0; i < entry_count_; ++i) {
        if (strcmp(paths_ + entries_[i].path_offset, path) == 0) return true;
    }
    return false;
}

}  // namespace aircannect
