#include "storage_manager.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board.h"
#include "debug_log.h"
#include "string_util.h"

#if AC_STORAGE_SDMMC_ENABLED
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include <SD_MMC.h>
#endif
#endif

#if AC_STORAGE_SPI_SD_ENABLED
#include <SD.h>
#include <SPI.h>
#endif

namespace aircannect {
namespace Storage {
namespace {

StorageStatus current;
bool initialized = false;

SemaphoreHandle_t sd_mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

void reset_status() {
    current = StorageStatus();
    copy_cstr(current.mount_point, sizeof(current.mount_point),
              AC_STORAGE_MOUNT_POINT);
    current.max_open_files = AC_STORAGE_MAX_OPEN_FILES;
}

fs::FS *active_fs() {
    if (!current.mounted) return nullptr;
    switch (current.type) {
        case StorageType::SdMmc:
#if AC_STORAGE_SDMMC_ENABLED && SOC_SDMMC_HOST_SUPPORTED
            return &SD_MMC;
#else
            return nullptr;
#endif
        case StorageType::SpiSd:
#if AC_STORAGE_SPI_SD_ENABLED
            return &SD;
#else
            return nullptr;
#endif
        case StorageType::None:
        default:
            return nullptr;
    }
}

void set_state(StorageType type,
               StorageState state,
               const char *error = nullptr) {
    current.type = type;
    current.state = state;
    current.mounted = state == StorageState::Mounted;
    copy_cstr(current.last_error, sizeof(current.last_error), error);
    current.last_checked_ms = millis();
}

#if (AC_STORAGE_SDMMC_ENABLED && SOC_SDMMC_HOST_SUPPORTED) || \
    AC_STORAGE_SPI_SD_ENABLED
const char *card_type_name(uint8_t type) {
    switch (type) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC";
        case CARD_NONE:
        default: return "none";
    }
}
#endif

#if AC_STORAGE_SDMMC_ENABLED && SOC_SDMMC_HOST_SUPPORTED
void update_sdmmc_capacity() {
    current.card_size_bytes = SD_MMC.cardSize();
    current.total_bytes = SD_MMC.totalBytes();
    current.used_bytes = SD_MMC.usedBytes();
    current.free_bytes = current.total_bytes > current.used_bytes
                             ? current.total_bytes - current.used_bytes
                             : 0;
    current.last_checked_ms = millis();
}
#endif

#if AC_STORAGE_SPI_SD_ENABLED
void update_spi_sd_capacity() {
    current.card_size_bytes = SD.cardSize();
    current.total_bytes = SD.totalBytes();
    current.used_bytes = SD.usedBytes();
    current.free_bytes = current.total_bytes > current.used_bytes
                             ? current.total_bytes - current.used_bytes
                             : 0;
    current.last_checked_ms = millis();
}
#endif

bool mount_sdmmc() {
#if AC_STORAGE_SDMMC_ENABLED
    current.configured = true;
    current.type = StorageType::SdMmc;
    current.width = AC_SDMMC_WIDTH;
#if !SOC_SDMMC_HOST_SUPPORTED
    set_state(StorageType::SdMmc, StorageState::Error,
              "SDMMC host not supported by this chip");
    return false;
#else
    if (AC_SDMMC_CLK_GPIO < 0 || AC_SDMMC_CMD_GPIO < 0 ||
        AC_SDMMC_D0_GPIO < 0) {
        set_state(StorageType::SdMmc, StorageState::Error,
                  "SDMMC pins are not configured");
        return false;
    }

    const bool mode1bit = AC_SDMMC_WIDTH <= 1;
    bool pins_ok = false;
    if (mode1bit) {
        pins_ok = SD_MMC.setPins(AC_SDMMC_CLK_GPIO, AC_SDMMC_CMD_GPIO,
                                 AC_SDMMC_D0_GPIO);
    } else if (AC_SDMMC_D1_GPIO >= 0 && AC_SDMMC_D2_GPIO >= 0 &&
               AC_SDMMC_D3_GPIO >= 0) {
        pins_ok = SD_MMC.setPins(AC_SDMMC_CLK_GPIO, AC_SDMMC_CMD_GPIO,
                                 AC_SDMMC_D0_GPIO, AC_SDMMC_D1_GPIO,
                                 AC_SDMMC_D2_GPIO, AC_SDMMC_D3_GPIO);
    }
    if (!pins_ok) {
        set_state(StorageType::SdMmc, StorageState::Error,
                  "SDMMC setPins failed");
        return false;
    }

    if (!SD_MMC.begin(AC_STORAGE_MOUNT_POINT, mode1bit, false,
                      AC_SDMMC_FREQ_KHZ, AC_STORAGE_MAX_OPEN_FILES)) {
        set_state(StorageType::SdMmc, StorageState::NotPresent,
                  "SDMMC mount failed");
        return false;
    }

    const uint8_t card_type = SD_MMC.cardType();
    copy_cstr(current.card_type, sizeof(current.card_type),
              card_type_name(card_type));
    if (card_type == CARD_NONE) {
        SD_MMC.end();
        set_state(StorageType::SdMmc, StorageState::NotPresent,
                  "no SDMMC card detected");
        return false;
    }

    update_sdmmc_capacity();
    set_state(StorageType::SdMmc, StorageState::Mounted);
    return true;
#endif
#else
    return false;
#endif
}

bool mount_spi_sd() {
#if AC_STORAGE_SPI_SD_ENABLED
    current.configured = true;
    current.type = StorageType::SpiSd;
    current.width = 1;
    if (AC_SPI_SD_CS_GPIO < 0 || AC_SPI_SD_SCK_GPIO < 0 ||
        AC_SPI_SD_MISO_GPIO < 0 || AC_SPI_SD_MOSI_GPIO < 0) {
        set_state(StorageType::SpiSd, StorageState::Error,
                  "SPI SD pins are not configured");
        return false;
    }

    SPI.begin(AC_SPI_SD_SCK_GPIO, AC_SPI_SD_MISO_GPIO,
              AC_SPI_SD_MOSI_GPIO, AC_SPI_SD_CS_GPIO);
    if (!SD.begin(AC_SPI_SD_CS_GPIO, SPI, AC_SPI_SD_FREQ_HZ,
                  AC_STORAGE_MOUNT_POINT, AC_STORAGE_MAX_OPEN_FILES)) {
        set_state(StorageType::SpiSd, StorageState::NotPresent,
                  "SPI SD mount failed");
        return false;
    }

    const uint8_t card_type = SD.cardType();
    copy_cstr(current.card_type, sizeof(current.card_type),
              card_type_name(card_type));
    if (card_type == CARD_NONE) {
        SD.end();
        set_state(StorageType::SpiSd, StorageState::NotPresent,
                  "no SPI SD card detected");
        return false;
    }

    update_spi_sd_capacity();
    set_state(StorageType::SpiSd, StorageState::Mounted);
    return true;
#else
    return false;
#endif
}

bool capacity_update_due(uint32_t now_ms) {
    if (!current.mounted) return false;
    return static_cast<int32_t>(now_ms - current.last_checked_ms) >=
        static_cast<int32_t>(AC_STORAGE_STATUS_POLL_MS);
}

void update_capacity_if_due(uint32_t now_ms) {
    if (!capacity_update_due(now_ms)) return;

    switch (current.type) {
        case StorageType::SdMmc:
#if AC_STORAGE_SDMMC_ENABLED && SOC_SDMMC_HOST_SUPPORTED
            update_sdmmc_capacity();
#endif
            break;
        case StorageType::SpiSd:
#if AC_STORAGE_SPI_SD_ENABLED
            update_spi_sd_capacity();
#endif
            break;
        case StorageType::None:
        default:
            break;
    }
}

}  // namespace

void lock() {
    SemaphoreHandle_t m = sd_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
}

void unlock() {
    SemaphoreHandle_t m = sd_mutex();
    if (m) xSemaphoreGiveRecursive(m);
}

void begin() {
    Guard g;
    if (initialized) return;
    initialized = true;
    remount();
}

void poll() {
    poll(true);
}

void poll(bool allow_capacity_update) {
    const bool need_begin = !initialized;
    const uint32_t now = millis();
    if (!need_begin &&
        (!allow_capacity_update || !capacity_update_due(now))) {
        return;
    }

    Guard g;
    if (!initialized) begin();
    if (allow_capacity_update) update_capacity_if_due(now);
}

bool remount() {
    Guard g;
    reset_status();

#if AC_STORAGE_SDMMC_ENABLED
    if (mount_sdmmc()) {
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "mounted type=%s card=%s mount=%s\n",
                  type_name(current.type), current.card_type,
                  current.mount_point);
        return true;
    }
#endif
#if AC_STORAGE_SPI_SD_ENABLED
    if (mount_spi_sd()) {
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "mounted type=%s card=%s mount=%s\n",
                  type_name(current.type), current.card_type,
                  current.mount_point);
        return true;
    }
#endif

    if (!current.configured) {
        set_state(StorageType::None, StorageState::Disabled,
                  "storage backend disabled");
    }
    Log::logf(CAT_STORAGE,
              current.state == StorageState::Error ? LOG_WARN : LOG_DEBUG,
              "unavailable type=%s state=%s error=%s\n",
              type_name(current.type), state_name(current.state),
              current.last_error[0] ? current.last_error : "--");
    return false;
}

StorageStatus status() {
    Guard g;
    if (!initialized) begin();
    return current;
}

bool mounted() {
    return status().mounted;
}

bool ensure_dir(const char *path) {
    if (!path || !*path) return false;
    Guard g;
    if (!initialized) begin();
    fs::FS *fs = active_fs();
    if (!fs) return false;
    if (fs->exists(path)) {
        File dir = fs->open(path);
        const bool ok = dir && dir.isDirectory();
        if (dir) dir.close();
        return ok;
    }
    return fs->mkdir(path);
}

bool exists(const char *path) {
    if (!path || !*path) return false;
    Guard g;
    if (!initialized) begin();
    fs::FS *fs = active_fs();
    return fs && fs->exists(path);
}

bool remove(const char *path) {
    if (!path || !*path) return false;
    Guard g;
    if (!initialized) begin();
    fs::FS *fs = active_fs();
    if (!fs) return false;
    if (fs->remove(path)) return true;
    return !fs->exists(path);
}

bool rmdir(const char *path) {
    if (!path || !*path) return false;
    Guard g;
    if (!initialized) begin();
    fs::FS *fs = active_fs();
    if (!fs) return false;
    if (fs->rmdir(path)) return true;
    return !fs->exists(path);
}

bool rename(const char *from, const char *to) {
    if (!from || !*from || !to || !*to) return false;
    Guard g;
    if (!initialized) begin();
    fs::FS *fs = active_fs();
    return fs && fs->rename(from, to);
}

File open(const char *path, const char *mode) {
    if (!path || !*path || !mode || !*mode) return File();
    Guard g;
    if (!initialized) begin();
    fs::FS *fs = active_fs();
    return fs ? fs->open(path, mode) : File();
}

const char *type_name(StorageType type) {
    switch (type) {
        case StorageType::SdMmc: return "sdmmc";
        case StorageType::SpiSd: return "spi-sd";
        case StorageType::None:
        default: return "none";
    }
}

const char *state_name(StorageState state) {
    switch (state) {
        case StorageState::NotPresent: return "not_present";
        case StorageState::Mounted: return "mounted";
        case StorageState::Error: return "error";
        case StorageState::Disabled:
        default: return "disabled";
    }
}

}  // namespace Storage
}  // namespace aircannect
