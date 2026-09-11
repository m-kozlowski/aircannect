#pragma once

#include <esp_flash_partitions.h>

namespace aircannect {

// Owned by FirmwareInstaller; only used after its normal OTA preparation.
class CoredumpPartitionUpdate {
public:
    struct Layout {
        uint32_t flash_size = 0;
        int entry_count = 0;
        bool entries_match = false;
        bool md5_present = false;
        bool tail_erased = false;
        uint32_t extra_address = 0;
        uint8_t extra_byte = 0;
    };

    bool prepare();
    bool apply();

    bool exists() const { return exists_; }
    bool safe_to_reboot() const { return safe_to_reboot_; }
    const char *error() const { return error_; }
    const Layout &layout() const { return layout_; }
    const esp_partition_info_t &entry(size_t index) const {
        return reinterpret_cast<const esp_partition_info_t *>(original_)[index];
    }

private:
    bool fail(const char *error);
    bool write_table(const void *table);

    alignas(4) unsigned char original_[ESP_PARTITION_TABLE_SIZE];
    alignas(4) unsigned char replacement_[ESP_PARTITION_TABLE_SIZE];
    alignas(4) unsigned char scratch_[ESP_PARTITION_TABLE_SIZE];
    uint32_t dump_offset_ = 0;
    bool prepared_ = false;
    bool exists_ = false;
    bool safe_to_reboot_ = true;
    const char *error_ = "";
    Layout layout_;
};

}  // namespace aircannect
