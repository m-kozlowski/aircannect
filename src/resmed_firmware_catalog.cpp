#include "resmed_firmware_catalog.h"

#include <algorithm>
#include <ctype.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "memory_manager.h"
#include "storage_path.h"

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

bool resmed_firmware_patched_bootloader_path(
    const char *bootloader_version,
    char *directory_out,
    size_t directory_out_size,
    char *path_out,
    size_t path_out_size) {
    if (!bootloader_version || !bootloader_version[0] || !directory_out ||
        directory_out_size == 0 || !path_out || path_out_size == 0) {
        return false;
    }

    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    char trailing = '\0';
    if (sscanf(bootloader_version, "%u.%u.%u%c",
               &major, &minor, &patch, &trailing) != 3) {
        return false;
    }

    const int directory_length = snprintf(
        directory_out, directory_out_size, "%s/%u.%u.%u",
        AC_RESMED_BOOTLOADER_REPOSITORY_PATH, major, minor, patch);
    if (directory_length <= 0 ||
        static_cast<size_t>(directory_length) >= directory_out_size) {
        return false;
    }

    const int path_length = snprintf(path_out, path_out_size, "%s/patched.bin",
                                     directory_out);
    return path_length > 0 &&
           static_cast<size_t>(path_length) < path_out_size;
}

ResmedFirmwareCatalogSnapshot::~ResmedFirmwareCatalogSnapshot() {
    Memory::free(entries_);
    Memory::free(paths_);
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
            Memory::calloc_large(entry_count, sizeof(Entry), false));
        result->paths_ = static_cast<char *>(
            Memory::calloc_large(path_bytes, sizeof(char), false));
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

        const char *filename = storage_basename_from_path(source.path);
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
