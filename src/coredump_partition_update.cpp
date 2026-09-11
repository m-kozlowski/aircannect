#include "coredump_partition_update.h"

#include <cstring>
#include <esp_flash.h>
#include <esp_flash_encrypt.h>
#include <esp_flash_internal.h>
#include <esp_partition.h>
#include <esp_rom_md5.h>
#include <esp_secure_boot.h>

namespace aircannect {
namespace {

constexpr uint32_t DumpSize = 0x10000;

bool erased(const unsigned char *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0xff) return false;
    }
    return true;
}

}  // namespace

bool CoredumpPartitionUpdate::fail(const char *error) {
    error_ = error;
    return false;
}

bool CoredumpPartitionUpdate::prepare() {
    prepared_ = false;
    exists_ = false;
    error_ = "";
    layout_ = {};

    if (esp_flash_encryption_enabled() || esp_secure_boot_enabled()) {
        return fail("secure_flash_unsupported");
    }
    if (ESP_PARTITION_TABLE_OFFSET != 0x8000) {
        return fail("partition_table_offset_unsupported");
    }

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) != ESP_OK) {
        return fail("flash_size_unavailable");
    }
    layout_.flash_size = flash_size;
    if (flash_size != 0x800000 && flash_size != 0x1000000) {
        return fail("flash_size_unsupported");
    }

    if (esp_flash_read(nullptr, original_, ESP_PARTITION_TABLE_OFFSET,
                       sizeof(original_)) != ESP_OK) {
        return fail("partition_table_read_failed");
    }

    const auto *entries =
        reinterpret_cast<const esp_partition_info_t *>(original_);
    int count = 0;
    if (esp_partition_table_verify(entries, false, &count) != ESP_OK) {
        return fail("partition_table_invalid");
    }
    layout_.entry_count = count;

    // The 8 MiB table is also used on boards with 16 MiB flash. Select the
    // candidate from app1's offset, then verify every entry against it below.
    const uint32_t layout_size = count >= 4 && entries[3].pos.offset == 0x400000
        ? 0x800000 : 0x1000000;
    const uint32_t slot_size = layout_size / 2 - 0x10000;
    dump_offset_ = layout_size - DumpSize;
    const esp_partition_info_t expected[] = {
        {ESP_PARTITION_MAGIC, 1, 2, {0x9000, 0x5000}, "nvs", 0},
        {ESP_PARTITION_MAGIC, 1, 0, {0xe000, 0x2000}, "otadata", 0},
        {ESP_PARTITION_MAGIC, 0, 0x10,
         {0x10000, slot_size}, "app0", 0},
        {ESP_PARTITION_MAGIC, 0, 0x11,
         {layout_size / 2, slot_size}, "app1", 0},
        {ESP_PARTITION_MAGIC, ESP_PARTITION_TYPE_DATA,
         ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
         {dump_offset_, DumpSize}, "coredump", 0},
    };

    // Accept only the historical AirCANnect tables, with an MD5 terminator.
    const bool entries_match = layout_size <= flash_size &&
        (count == 4 || count == 5) &&
        memcmp(entries, expected, count * sizeof(*entries)) == 0;
    const bool md5_present = entries[count].magic == ESP_PARTITION_MAGIC_MD5;
    const size_t end = (count + 1) * sizeof(*entries);
    const bool tail_erased = erased(original_ + end, sizeof(original_) - end);

    layout_.entries_match = entries_match;
    layout_.md5_present = md5_present;
    layout_.tail_erased = tail_erased;
    if (!tail_erased) {
        size_t offset = end;
        while (original_[offset] == 0xff) ++offset;
        layout_.extra_address = ESP_PARTITION_TABLE_OFFSET + offset;
        layout_.extra_byte = original_[offset];
    }

    if (!entries_match || !md5_present || !tail_erased) {
        return fail("partition_layout_unsupported");
    }

    exists_ = count == 5;
    if (exists_) return true;

    for (uint32_t offset = 0; offset < DumpSize; offset += sizeof(scratch_)) {
        if (esp_flash_read(nullptr, scratch_, dump_offset_ + offset,
                           sizeof(scratch_)) != ESP_OK) {
            return fail("coredump_area_read_failed");
        }
        if (!erased(scratch_, sizeof(scratch_))) {
            return fail("coredump_area_not_empty");
        }
    }

    memcpy(replacement_, original_, sizeof(original_));
    memcpy(replacement_ + 4 * sizeof(*entries), &expected[4], sizeof(*entries));

    auto *md5 = replacement_ + 5 * sizeof(*entries);
    memset(md5, 0xff, sizeof(*entries));
    const uint16_t magic = ESP_PARTITION_MAGIC_MD5;
    memcpy(md5, &magic, sizeof(magic));

    md5_context_t context;
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, replacement_, 5 * sizeof(*entries));
    esp_rom_md5_final(md5 + ESP_PARTITION_MD5_OFFSET, &context);

    prepared_ = true;
    return true;
}

bool CoredumpPartitionUpdate::write_table(const void *table) {
    return esp_flash_erase_region(nullptr, ESP_PARTITION_TABLE_OFFSET,
                                  ESP_PARTITION_TABLE_SIZE) == ESP_OK &&
           esp_flash_write(nullptr, table, ESP_PARTITION_TABLE_OFFSET,
                           ESP_PARTITION_TABLE_SIZE) == ESP_OK &&
           esp_flash_read(nullptr, scratch_, ESP_PARTITION_TABLE_OFFSET,
                          ESP_PARTITION_TABLE_SIZE) == ESP_OK &&
           memcmp(scratch_, table, ESP_PARTITION_TABLE_SIZE) == 0;
}

bool CoredumpPartitionUpdate::apply() {
    if (!prepared_) return fail("partition_update_not_prepared");
    prepared_ = false;

    if (esp_flash_read(nullptr, scratch_, ESP_PARTITION_TABLE_OFFSET,
                       sizeof(scratch_)) != ESP_OK ||
        memcmp(scratch_, original_, sizeof(original_)) != 0) {
        return fail("partition_table_changed");
    }

    // The free area was already verified erased. Only the table needs writing.
    if (esp_flash_set_dangerous_write_protection(
            esp_flash_default_chip, false) != ESP_OK) {
        return fail("partition_table_unlock_failed");
    }

    const bool written = write_table(replacement_);
    safe_to_reboot_ = written || write_table(original_);
    const esp_err_t protection = esp_flash_set_dangerous_write_protection(
        esp_flash_default_chip, true);

    if (!safe_to_reboot_) return fail("partition_table_restore_failed_no_reboot");
    if (protection != ESP_OK) return fail("partition_table_protection_failed");
    if (!written) return fail("partition_table_write_failed_restored");
    return true;
}

}  // namespace aircannect
