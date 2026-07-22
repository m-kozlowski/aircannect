#include "storage_internal.h"

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

StorageStatus owner_status;
StorageStatus published_status;
bool initialized = false;

SemaphoreHandle_t status_mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

bool lock_status(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = status_mutex();
    return mutex && xSemaphoreTake(mutex, wait) == pdTRUE;
}

void unlock_status() {
    SemaphoreHandle_t mutex = status_mutex();
    if (mutex) xSemaphoreGive(mutex);
}

void publish_status() {
    if (!lock_status()) return;
    published_status = owner_status;
    unlock_status();
}

void reset_status() {
    owner_status = StorageStatus();
    copy_cstr(owner_status.mount_point, sizeof(owner_status.mount_point),
              AC_STORAGE_MOUNT_POINT);
    owner_status.max_open_files = AC_STORAGE_MAX_OPEN_FILES;
}

fs::FS *active_fs() {
    if (!owner_status.mounted) return nullptr;
    switch (owner_status.type) {
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
    owner_status.type = type;
    owner_status.state = state;
    owner_status.mounted = state == StorageState::Mounted;
    copy_cstr(owner_status.last_error, sizeof(owner_status.last_error), error);
    owner_status.last_checked_ms = millis();
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
    owner_status.card_size_bytes = SD_MMC.cardSize();
    owner_status.total_bytes = SD_MMC.totalBytes();
    owner_status.used_bytes = SD_MMC.usedBytes();
    owner_status.free_bytes =
        owner_status.total_bytes > owner_status.used_bytes
            ? owner_status.total_bytes - owner_status.used_bytes
            : 0;
    owner_status.last_checked_ms = millis();
}
#endif

#if AC_STORAGE_SPI_SD_ENABLED
void update_spi_sd_capacity() {
    owner_status.card_size_bytes = SD.cardSize();
    owner_status.total_bytes = SD.totalBytes();
    owner_status.used_bytes = SD.usedBytes();
    owner_status.free_bytes =
        owner_status.total_bytes > owner_status.used_bytes
            ? owner_status.total_bytes - owner_status.used_bytes
            : 0;
    owner_status.last_checked_ms = millis();
}
#endif

bool mount_sdmmc() {
#if AC_STORAGE_SDMMC_ENABLED
    owner_status.configured = true;
    owner_status.type = StorageType::SdMmc;
    owner_status.width = AC_SDMMC_WIDTH;
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
    copy_cstr(owner_status.card_type, sizeof(owner_status.card_type),
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
    owner_status.configured = true;
    owner_status.type = StorageType::SpiSd;
    owner_status.width = 1;
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
    copy_cstr(owner_status.card_type, sizeof(owner_status.card_type),
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
    if (!owner_status.mounted) return false;
    return static_cast<int32_t>(now_ms - owner_status.last_checked_ms) >=
        static_cast<int32_t>(AC_STORAGE_STATUS_POLL_MS);
}

void update_capacity_if_due(uint32_t now_ms) {
    if (!capacity_update_due(now_ms)) return;

    switch (owner_status.type) {
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

bool mount_storage() {
    reset_status();

#if AC_STORAGE_SDMMC_ENABLED
    if (mount_sdmmc()) {
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "mounted type=%s card=%s mount=%s\n",
                  type_name(owner_status.type), owner_status.card_type,
                  owner_status.mount_point);
        return true;
    }
#endif
#if AC_STORAGE_SPI_SD_ENABLED
    if (mount_spi_sd()) {
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "mounted type=%s card=%s mount=%s\n",
                  type_name(owner_status.type), owner_status.card_type,
                  owner_status.mount_point);
        return true;
    }
#endif

    if (!owner_status.configured) {
        set_state(StorageType::None, StorageState::Disabled,
                  "storage backend disabled");
    }
    Log::logf(CAT_STORAGE,
              owner_status.state == StorageState::Error ? LOG_WARN : LOG_DEBUG,
              "unavailable type=%s state=%s error=%s\n",
              type_name(owner_status.type), state_name(owner_status.state),
              owner_status.last_error[0] ? owner_status.last_error : "--");
    return false;
}

}  // namespace

void begin() {
    if (initialized) return;

    initialized = true;
    (void)mount_storage();
    publish_status();
}

bool poll(bool allow_capacity_update) {
    if (!initialized || !allow_capacity_update) return false;

    const uint32_t now_ms = millis();
    if (!capacity_update_due(now_ms)) return false;

    update_capacity_if_due(now_ms);
    publish_status();
    return true;
}

bool retry_mount() {
    if (!initialized) initialized = true;

    if (owner_status.mounted) return true;

    const bool mounted_now = mount_storage();
    publish_status();
    return mounted_now;
}

StorageStatus status() {
    StorageStatus out;
    if (!lock_status()) return out;

    out = published_status;
    unlock_status();
    return out;
}

bool try_status(StorageStatus &out) {
    if (!lock_status(0)) return false;

    out = published_status;
    unlock_status();
    return true;
}

bool mounted() {
    return status().mounted;
}

bool ensure_dir(const char *path) {
    if (!initialized || !path || !*path) return false;
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
    if (!initialized || !path || !*path) return false;
    fs::FS *fs = active_fs();
    return fs && fs->exists(path);
}

bool remove(const char *path) {
    if (!initialized || !path || !*path) return false;
    fs::FS *fs = active_fs();
    if (!fs) return false;
    if (fs->remove(path)) return true;
    return !fs->exists(path);
}

bool rmdir(const char *path) {
    if (!initialized || !path || !*path) return false;
    fs::FS *fs = active_fs();
    if (!fs) return false;
    if (fs->rmdir(path)) return true;
    return !fs->exists(path);
}

bool rename(const char *from, const char *to) {
    if (!initialized || !from || !*from || !to || !*to) return false;
    fs::FS *fs = active_fs();
    return fs && fs->rename(from, to);
}

File open(const char *path, const char *mode) {
    if (!initialized || !path || !*path || !mode || !*mode) return File();
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
