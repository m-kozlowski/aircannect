#pragma once

#include <stdint.h>

namespace aircannect {

enum class StorageType : uint8_t {
    None,
    SdMmc,
    SpiSd,
};

enum class StorageState : uint8_t {
    Disabled,
    NotPresent,
    Mounted,
    Error,
};

struct StorageStatus {
    bool configured = false;
    bool mounted = false;
    StorageType type = StorageType::None;
    StorageState state = StorageState::Disabled;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t card_size_bytes = 0;
    // FAT12/16/32 allow at most 128 sectors per allocation unit.
    // Conservative bound, not the card's measured cluster size.
    uint32_t allocation_unit_bound = 0;
    uint32_t last_checked_ms = 0;
    uint8_t max_open_files = 0;
    uint8_t width = 0;
    uint32_t requested_hz = 0;
    // Confirmed transfer clock; zero when unavailable.
    uint32_t actual_hz = 0;
    char mount_point[16] = {};
    char card_type[16] = {};
    char last_error[96] = {};
};

struct StorageSpaceSnapshot {
    bool known = false;
    uint64_t free_bytes = 0;
    uint32_t allocation_unit_bound = 0;

    bool fits(uint64_t bytes, uint64_t reserve) const {
        return known && free_bytes >= reserve && bytes <= free_bytes - reserve;
    }

    uint64_t allocation_bytes(uint64_t bytes) const {
        if (!allocation_unit_bound || bytes > UINT64_MAX - allocation_unit_bound) {
            return UINT64_MAX;
        }
        return ((bytes + allocation_unit_bound - 1) / allocation_unit_bound) *
               allocation_unit_bound;
    }
};

namespace Storage {

void begin();

StorageStatus status();
StorageSpaceSnapshot space_snapshot();
bool try_status(StorageStatus &out);
bool mounted();

const char *type_name(StorageType type);
const char *state_name(StorageState state);

}  // namespace Storage
}  // namespace aircannect
