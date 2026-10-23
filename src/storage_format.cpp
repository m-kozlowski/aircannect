#include "storage_internal.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include <esp_err.h>

#if AC_STORAGE_SDMMC_ENABLED
#include <soc/soc_caps.h>
#if SOC_SDMMC_HOST_SUPPORTED
#define AC_FORMAT_SDMMC 1
#include <ff.h>
#include <driver/sdmmc_host.h>
#include <diskio_sdmmc.h>
#include <sdmmc_cmd.h>
#else
#define AC_FORMAT_SDMMC 0
#endif
#else
#define AC_FORMAT_SDMMC 0
#endif

#if !AC_FORMAT_SDMMC && AC_STORAGE_SPI_SD_ENABLED
#define AC_FORMAT_SPI_SD 1
#include <SD.h>
#include <SPI.h>
#include <sd_diskio.h>
#else
#define AC_FORMAT_SPI_SD 0
#endif

#if AC_FORMAT_SDMMC || AC_FORMAT_SPI_SD
#include <diskio.h>
#include <diskio_impl.h>
#include <esp_heap_caps.h>
#include <ff.h>

namespace aircannect::Storage {
namespace {

constexpr size_t FormatWorkBytes = 4096;
constexpr uint32_t SectorBytes = 512;
static_assert(FF_MAX_SS <= FormatWorkBytes, "SD format work buffer too small");

void set_format_error(char *error, size_t size, const char *phase, int result) {
    if (!error || size == 0) return;

    snprintf(error, size, "format %s failed (%d)", phase, result);
}

}  // namespace

bool format_card_on_boot(char *error, size_t size) {
    if (error && size > 0) error[0] = '\0';

    const StorageStatus current = status();
    if (current.configured || current.mounted) {
        set_format_error(error, size, "boot-only guard", ESP_ERR_INVALID_STATE);
        return false;
    }

    uint8_t *work = nullptr;
    BYTE pdrv = FF_DRV_NOT_USED;
    DWORD sector_count = 0;
    PARTITION previous_partition = {};
    bool partition_mapping_changed = false;
    bool disk_registered = false;
#if AC_FORMAT_SDMMC
    bool host_started = false;
#elif AC_FORMAT_SPI_SD
    bool spi_started = false;
#endif
    const char *phase = "buffer";
    int result = ESP_OK;

#if AC_FORMAT_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    sdmmc_card_t card = {};
#endif

    const bool formatted = [&]() {
        work = static_cast<uint8_t *>(heap_caps_malloc(
            FormatWorkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!work) {
            result = ESP_ERR_NO_MEM;
            return false;
        }

#if AC_FORMAT_SDMMC
        phase = "host state";
        sdmmc_host_state_t host_state = {};
        result = sdmmc_host_get_state(&host_state);
        if (result != ESP_OK) return false;
        if (host_state.host_initialized) {
            result = ESP_ERR_INVALID_STATE;
            return false;
        }

        if (AC_SDMMC_CLK_GPIO < 0 || AC_SDMMC_CMD_GPIO < 0 ||
            AC_SDMMC_D0_GPIO < 0 ||
            (AC_SDMMC_WIDTH > 1 &&
             (AC_SDMMC_D1_GPIO < 0 || AC_SDMMC_D2_GPIO < 0 ||
              AC_SDMMC_D3_GPIO < 0))) {
            phase = "SDMMC pins";
            result = ESP_ERR_INVALID_ARG;
            return false;
        }

        phase = "drive";
        result = ff_diskio_get_drive(&pdrv);
        if (result != ESP_OK || pdrv == FF_DRV_NOT_USED) {
            if (result == ESP_OK) result = ESP_ERR_NOT_FOUND;
            return false;
        }

        host.flags = AC_SDMMC_WIDTH <= 1
            ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;
        host.max_freq_khz = AC_SDMMC_FREQ_KHZ;
        slot.width = AC_SDMMC_WIDTH;
        slot.clk = static_cast<gpio_num_t>(AC_SDMMC_CLK_GPIO);
        slot.cmd = static_cast<gpio_num_t>(AC_SDMMC_CMD_GPIO);
        slot.d0 = static_cast<gpio_num_t>(AC_SDMMC_D0_GPIO);
        slot.d1 = static_cast<gpio_num_t>(
            AC_SDMMC_WIDTH <= 1 ? -1 : AC_SDMMC_D1_GPIO);
        slot.d2 = static_cast<gpio_num_t>(
            AC_SDMMC_WIDTH <= 1 ? -1 : AC_SDMMC_D2_GPIO);
        slot.d3 = static_cast<gpio_num_t>(
            AC_SDMMC_WIDTH <= 1 ? -1 : AC_SDMMC_D3_GPIO);
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        phase = "host init";
        result = sdmmc_host_init();
        if (result != ESP_OK) return false;
        host_started = true;

        phase = "slot init";
        result = sdmmc_host_init_slot(host.slot, &slot);
        if (result != ESP_OK) return false;

        phase = "card init";
        result = sdmmc_card_init(&host, &card);
        if (result != ESP_OK) return false;

        phase = "card type";
        // FatFs may map CTRL_TRIM to a whole-volume MMC erase operation.
        if (card.is_mmc || card.is_sdio) {
            result = ESP_ERR_NOT_SUPPORTED;
            return false;
        }

        phase = "card geometry";
        if (card.csd.sector_size != SectorBytes ||
            card.csd.capacity <= 100) {
            result = ESP_ERR_NOT_SUPPORTED;
            return false;
        }
        sector_count = card.csd.capacity;

        ff_diskio_register_sdmmc(pdrv, &card);
        disk_registered = true;

#elif AC_FORMAT_SPI_SD
        if (AC_SPI_SD_CS_GPIO < 0 || AC_SPI_SD_SCK_GPIO < 0 ||
            AC_SPI_SD_MISO_GPIO < 0 || AC_SPI_SD_MOSI_GPIO < 0) {
            phase = "SPI SD pins";
            result = ESP_ERR_INVALID_ARG;
            return false;
        }

        phase = "SPI init";
        if (!SPI.begin(AC_SPI_SD_SCK_GPIO, AC_SPI_SD_MISO_GPIO,
                       AC_SPI_SD_MOSI_GPIO, AC_SPI_SD_CS_GPIO)) {
            result = ESP_FAIL;
            return false;
        }
        spi_started = true;

        // Use Arduino SD's registered FatFs disk driver without mounting a VFS.
        phase = "drive";
        pdrv = sdcard_init(AC_SPI_SD_CS_GPIO, &SPI, AC_SPI_SD_FREQ_HZ);
        if (pdrv == FF_DRV_NOT_USED) {
            result = ESP_ERR_NOT_FOUND;
            return false;
        }
        disk_registered = true;

        phase = "card init";
        const DSTATUS disk_status = disk_initialize(pdrv);
        if (disk_status != 0) {
            result = disk_status;
            return false;
        }

        phase = "card type";
        const sdcard_type_t card_type = sdcard_type(pdrv);
        if (card_type != CARD_SD && card_type != CARD_SDHC) {
            result = card_type == CARD_MMC
                ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
            return false;
        }

        phase = "card geometry";
        const uint32_t reported_sectors = sdcard_num_sectors(pdrv);
        const uint32_t reported_sector_size = sdcard_sector_size(pdrv);
        if (reported_sectors <= 100 ||
            reported_sector_size != SectorBytes) {
            result = ESP_ERR_NOT_SUPPORTED;
            return false;
        }
        sector_count = reported_sectors;
#endif

        phase = "partition mapping";
        previous_partition = VolToPart[pdrv];
        VolToPart[pdrv] = {pdrv, 1};
        partition_mapping_changed = true;

        const LBA_t partitions[] = {100, 0, 0, 0};
        phase = "partition";
        const FRESULT partition_result = f_fdisk(pdrv, partitions, work);
        if (partition_result != FR_OK) {
            result = partition_result;
            return false;
        }

        memset(work, 0, SectorBytes);
        phase = "GPT headers";
        DRESULT write_result = disk_write(pdrv, work, 1, 1);
        if (write_result != RES_OK) {
            result = write_result;
            return false;
        }

        write_result = disk_write(pdrv, work, sector_count - 1, 1);
        if (write_result != RES_OK) {
            result = write_result;
            return false;
        }

        const MKFS_PARM options = {FM_FAT32, 2, 0, 0, 0};
        char drive[] = {static_cast<char>('0' + pdrv), ':', '\0'};
        phase = "FAT32";
        const FRESULT format_result =
            f_mkfs(drive, &options, work, FormatWorkBytes);
        result = format_result;
        return format_result == FR_OK;
    }();

    if (partition_mapping_changed) {
        VolToPart[pdrv] = previous_partition;
    }

    if (disk_registered) {
#if AC_FORMAT_SDMMC
        ff_diskio_unregister(pdrv);
#elif AC_FORMAT_SPI_SD
        phase = formatted ? "SPI unregister" : phase;
        const uint8_t uninit_result = sdcard_uninit(pdrv);
        if (uninit_result != 0 && formatted) {
            result = uninit_result;
        }
#endif
    }

#if AC_FORMAT_SDMMC
    if (host_started) {
        const esp_err_t shutdown_result = sdmmc_host_deinit();
        if (shutdown_result != ESP_OK && formatted) {
            phase = "host shutdown";
            result = shutdown_result;
        }
    }
#elif AC_FORMAT_SPI_SD
    if (spi_started) SPI.end();
#endif

    heap_caps_free(work);

    const bool success = formatted && result == ESP_OK;
    if (!success) set_format_error(error, size, phase, result);
    return success;
}

}  // namespace aircannect::Storage

#else

namespace aircannect::Storage {

bool format_card_on_boot(char *error, size_t size) {
    if (error && size > 0) {
        snprintf(error, size, "format backend disabled (%d)",
                 ESP_ERR_NOT_SUPPORTED);
    }
    return false;
}

}  // namespace aircannect::Storage

#endif
