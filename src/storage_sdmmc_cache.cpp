#include "board_storage.h"

#ifdef ARDUINO
#include <ff.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <cstring>

#include "memory_manager.h"

extern "C" void __real_ff_diskio_register(BYTE, const ff_diskio_impl_t *);
extern "C" esp_err_t __real_sdmmc_wait_for_idle(sdmmc_card_t *, uint32_t);

namespace aircannect::Storage {
#if AC_STORAGE_SDMMC_ENABLED
namespace {

// Local FAT I/O is serialized by FatFs. USB raw access begins after unregister.
constexpr size_t SectorBytes = 512;
constexpr size_t Slots = 64;
struct Sector {
    LBA_t lba;
    uint32_t age;
};

Sector sectors[Slots];
using SectorBytesBuffer = BYTE[SectorBytes];
SectorBytesBuffer *contents = nullptr;
uint32_t clock_tick = 0;
BYTE drive = 0xff;
bool mounting = false;
bool card_wait_failed = false;
ff_diskio_impl_t physical;

void invalidate() {
    for (size_t i = 0; i < Slots; ++i) sectors[i].age = 0;
    clock_tick = 0;
}

DSTATUS initialize(BYTE disk) {
    invalidate();
    return physical.init(disk);
}

DSTATUS status(BYTE disk) {
    const auto result = physical.status(disk);
    if (result) invalidate();
    return result;
}

DRESULT read(BYTE disk, BYTE *output, DWORD lba, UINT count) {
    Sector *victim = nullptr;
    if (count == 1) {
        if (clock_tick == UINT32_MAX) invalidate();
        victim = sectors;

        for (size_t i = 0; i < Slots; ++i) {
            auto &slot = sectors[i];
            if (slot.age && slot.lba == lba) {
                memcpy(output, contents[i], SectorBytes);
                slot.age = ++clock_tick;
                return RES_OK;
            }
            if (slot.age < victim->age) victim = &slot;
        }
    }

    card_wait_failed = false;
    const auto io = physical.read(disk, output, lba, count);
    const auto result = card_wait_failed ? RES_ERROR : io;
    if (result != RES_OK) {
        invalidate();
    } else if (victim) {
        memcpy(contents[victim - sectors], output, SectorBytes);
        victim->lba = lba;
        victim->age = ++clock_tick;
    }
    return result;
}

DRESULT write(BYTE disk, const BYTE *input, DWORD lba, UINT count) {
    card_wait_failed = false;
    const auto io = physical.write(disk, input, lba, count);
    const auto result = card_wait_failed ? RES_ERROR : io;
    if (result != RES_OK || clock_tick > UINT32_MAX - Slots) {
        invalidate();
        return result;
    }

    for (size_t i = 0; i < Slots; ++i) {
        auto &slot = sectors[i];
        if (slot.age && slot.lba >= lba && slot.lba - lba < count) {
            memcpy(contents[i], input + (slot.lba - lba) * SectorBytes, SectorBytes);
            slot.age = ++clock_tick;
        }
    }
    return result;
}

DRESULT ioctl(BYTE disk, BYTE command, void *data) {
    const auto result = physical.ioctl(disk, command, data);
    if (command == CTRL_TRIM || result != RES_OK) invalidate();
    return result;
}

const ff_diskio_impl_t cached = {initialize, status, read, write, ioctl};
}  // namespace

void set_sdmmc_mount_in_progress(bool value) {
    mounting = value;
}
#endif

extern "C" esp_err_t __wrap_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status) {
    const auto error = __real_sdmmc_wait_for_idle(card, status);
#if AC_STORAGE_SDMMC_ENABLED
    // IDF can return the data command's earlier success on a failed CMD13.
    if (error != ESP_OK && drive != 0xff && ff_diskio_get_pdrv_card(card) == drive) {
        card_wait_failed = true;
        invalidate();
    }
#endif
    return error;
}

extern "C" void __wrap_ff_diskio_register(BYTE disk, const ff_diskio_impl_t *ops) {
#if AC_STORAGE_SDMMC_ENABLED
    if (disk == drive) {
        Memory::free(contents);
        contents = nullptr;
        drive = 0xff;
    }

    WORD bytes = 0;
    if (mounting && ops && ops->ioctl(disk, GET_SECTOR_SIZE, &bytes) == RES_OK &&
        bytes == SectorBytes) {
        contents = static_cast<SectorBytesBuffer *>(Memory::alloc_large(Slots * SectorBytes, false));
        if (contents) {
            physical = *ops;
            drive = disk;
            invalidate();
            ops = &cached;
        }
    }
#endif

    __real_ff_diskio_register(disk, ops);
}
}  // namespace aircannect::Storage
#endif
