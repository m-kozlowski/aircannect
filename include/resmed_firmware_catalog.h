#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "storage_scan_port.h"

namespace aircannect {

static constexpr const char *AC_RESMED_FIRMWARE_REPOSITORY_PATH =
    "/aircannect/resmed-firmware";

enum class ResmedFirmwareNameHint : uint8_t {
    Abc,
    Raw,
    Unsupported,
};

struct ResmedFirmwareEntryView {
    const char *path = nullptr;
    const char *filename = nullptr;
    ResmedFirmwareNameHint name_hint = ResmedFirmwareNameHint::Unsupported;
    uint64_t size = 0;
    uint64_t modified = 0;
};

const char *resmed_firmware_name_hint_name(ResmedFirmwareNameHint hint);
ResmedFirmwareNameHint resmed_firmware_name_hint_for_filename(
    const char *filename);

class ResmedFirmwareCatalogSnapshot {
public:
    ~ResmedFirmwareCatalogSnapshot();

    static std::shared_ptr<const ResmedFirmwareCatalogSnapshot> build(
        const StorageScanSnapshot &scan,
        uint32_t revision);

    size_t size() const { return entry_count_; }
    uint32_t revision() const { return revision_; }
    bool truncated() const { return truncated_; }
    bool entry(size_t index, ResmedFirmwareEntryView &out) const;
    bool contains_file(const char *path) const;

private:
    static constexpr size_t MaxEntries = 512;

    struct Entry {
        uint64_t size = 0;
        uint64_t modified = 0;
        uint32_t path_offset = 0;
        uint32_t filename_offset = 0;
        ResmedFirmwareNameHint name_hint =
            ResmedFirmwareNameHint::Unsupported;
    };

    Entry *entries_ = nullptr;
    size_t entry_count_ = 0;
    char *paths_ = nullptr;
    size_t paths_length_ = 0;
    uint32_t revision_ = 0;
    bool truncated_ = false;
};

}  // namespace aircannect
