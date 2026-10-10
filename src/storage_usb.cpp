#include "storage_usb.h"

#include <atomic>
#include <algorithm>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board.h"
#include "debug_log.h"
#include "storage_access.h"
#include "storage_internal.h"
#include "string_util.h"

#if AC_STORAGE_SDMMC_ENABLED && !ARDUINO_USB_MODE
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <sdmmc_cmd.h>
#include <esp32-hal-tinyusb.h>
#define AC_USB_SD_SUPPORTED 1
#else
#define AC_USB_SD_SUPPORTED 0
#endif

namespace aircannect::StorageUsb {
namespace {

std::atomic<StorageUsbState> state{StorageUsbState::Local};
std::atomic<bool> entry_allowed{false};
std::atomic<bool> stopped{false};
StaticSemaphore_t mutex_storage;
SemaphoreHandle_t mutex = nullptr;
StaticSemaphore_t media_mutex_storage;
SemaphoreHandle_t media_mutex = nullptr;
StorageUsbStatus snapshot;
void (*wake)() = nullptr;

#if AC_USB_SD_SUPPORTED
sdmmc_card_t card{};
bool raw_active = false;
bool prevent_removal = false;
uint8_t *sector_buffer = nullptr;
static constexpr size_t SectorBytes = 512;
static constexpr size_t TransferBytes = 4096;

uint16_t descriptor(uint8_t *destination, uint8_t *interface) {
    const uint8_t endpoint = tinyusb_get_free_duplex_endpoint();
    if (!endpoint) return 0;

    const uint8_t data[] = {
        TUD_MSC_DESCRIPTOR(*interface,
            tinyusb_add_string_descriptor("AirCANnect SD"),
            endpoint, static_cast<uint8_t>(0x80 | endpoint),
            CFG_TUD_ENDOINT_SIZE)
    };
    ++*interface;
    memcpy(destination, data, sizeof(data));
    return sizeof(data);
}

// Arduino starts USB before setup(). Register the fixed composite interface now.
const bool registered = tinyusb_enable_interface(
    USB_INTERFACE_MSC, TUD_MSC_DESC_LEN, descriptor);
#endif

void publish_locked(StorageUsbState next, const char *error = nullptr) {
    snapshot.state = next;
    ++snapshot.revision;
    copy_cstr(snapshot.error, sizeof(snapshot.error), error);
    state.store(next, std::memory_order_release);
}

}  // namespace

void begin(void (*wake_storage)()) {
    if (mutex) return;

    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    media_mutex = xSemaphoreCreateMutexStatic(&media_mutex_storage);
    wake = wake_storage;
#if AC_USB_SD_SUPPORTED
    snapshot.supported = registered;
#endif
}

StorageUsbStatus status() {
    if (!mutex) return {};

    xSemaphoreTake(mutex, portMAX_DELAY);
    const StorageUsbStatus result = snapshot;
    xSemaphoreGive(mutex);
    return result;
}

bool suspended() { return state.load() != StorageUsbState::Local; }
void set_entry_allowed(bool allowed) { entry_allowed.store(allowed); }
void set_producers_stopped(bool value) {
    if (stopped.exchange(value) != value && wake) wake();
}
bool producers_stopped() { return stopped.load(); }
void seal_local_requests() { storage_local_requests_enabled.store(false); }

bool request(bool enabled) {
    if (!mutex || xSemaphoreTake(mutex, 0) != pdTRUE) return false;

    bool accepted = false;
    if (enabled && snapshot.supported &&
        snapshot.state == StorageUsbState::Local && entry_allowed.load()) {
        stopped.store(false);
        publish_locked(StorageUsbState::Preparing);
        accepted = true;
    } else if (!enabled && (snapshot.state == StorageUsbState::Shared ||
                            snapshot.state == StorageUsbState::Error ||
                            snapshot.state == StorageUsbState::Preparing)) {
        publish_locked(StorageUsbState::Returning);
        accepted = true;
    }

    // Manual return asserts that the operator has unmounted the host filesystem.
    xSemaphoreGive(mutex);
    if (accepted && wake) wake();
    return accepted;
}

void reject_entry(const char *error) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (snapshot.state == StorageUsbState::Preparing) {
        publish_locked(StorageUsbState::Local, error);
    }
    xSemaphoreGive(mutex);
}

bool share() {
#if AC_USB_SD_SUPPORTED
    xSemaphoreTake(media_mutex, portMAX_DELAY);
    if (state.load() != StorageUsbState::Preparing) {
        xSemaphoreGive(media_mutex);
        return false;
    }

    Storage::unmount();
    sector_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(TransferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    esp_err_t error = sector_buffer ? sdmmc_host_init() : ESP_ERR_NO_MEM;
    raw_active = error == ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = AC_SDMMC_FREQ_KHZ;
    host.flags = AC_SDMMC_WIDTH == 1 ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = AC_SDMMC_WIDTH;
    slot.clk = static_cast<gpio_num_t>(AC_SDMMC_CLK_GPIO);
    slot.cmd = static_cast<gpio_num_t>(AC_SDMMC_CMD_GPIO);
    slot.d0 = static_cast<gpio_num_t>(AC_SDMMC_D0_GPIO);
    slot.d1 = static_cast<gpio_num_t>(AC_SDMMC_D1_GPIO);
    slot.d2 = static_cast<gpio_num_t>(AC_SDMMC_D2_GPIO);
    slot.d3 = static_cast<gpio_num_t>(AC_SDMMC_D3_GPIO);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (error == ESP_OK) error = sdmmc_host_init_slot(host.slot, &slot);
    if (error == ESP_OK) error = sdmmc_card_init(&host, &card);
    if (error == ESP_OK && (card.csd.sector_size != SectorBytes ||
                           card.csd.capacity == 0)) {
        error = ESP_ERR_NOT_SUPPORTED;
    }

    prevent_removal = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool shared = error == ESP_OK && snapshot.state == StorageUsbState::Preparing;
    publish_locked(shared ? StorageUsbState::Shared : StorageUsbState::Returning,
                   error == ESP_OK ? nullptr : esp_err_to_name(error));
    xSemaphoreGive(mutex);
    xSemaphoreGive(media_mutex);

    Log::logf(CAT_STORAGE, error == ESP_OK ? LOG_INFO : LOG_ERROR,
              "[USB] %s error=%s", shared ? "SD shared read/write" :
                  error == ESP_OK ? "handoff cancelled" : "handoff failed",
              esp_err_to_name(error));
    return shared;
#else
    return false;
#endif
}

bool poll() {
    if (state.load() != StorageUsbState::Returning || !stopped.load()) return false;

    xSemaphoreTake(media_mutex, portMAX_DELAY);
#if AC_USB_SD_SUPPORTED
    if (raw_active) {
        const esp_err_t error = sdmmc_host_deinit();
        if (error != ESP_OK) {
            xSemaphoreGive(media_mutex);
            fail(esp_err_to_name(error));
            return false;
        }
    }
    raw_active = false;
    heap_caps_free(sector_buffer);
    sector_buffer = nullptr;
    card = {};
#endif
    xSemaphoreGive(media_mutex);
    return true;
}

void restored(bool success) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    publish_locked(success ? StorageUsbState::Restoring : StorageUsbState::Error,
                   success ? nullptr : "SD remount failed; no format attempted");
    xSemaphoreGive(mutex);

    Log::logf(CAT_STORAGE, success ? LOG_INFO : LOG_ERROR,
              "[USB] %s", success ? "SD returned to application"
                                  : "SD remount failed");
}

void fail(const char *error) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    publish_locked(StorageUsbState::Error, error);
    xSemaphoreGive(mutex);
    Log::logf(CAT_STORAGE, LOG_ERROR, "[USB] handoff stopped error=%s", error);
}

void resume_local() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (snapshot.state == StorageUsbState::Restoring) {
        storage_local_requests_enabled.store(true);
        publish_locked(StorageUsbState::Local);
    }
    xSemaphoreGive(mutex);
    if (wake) wake();
}

const char *state_name(StorageUsbState value) {
    switch (value) {
        case StorageUsbState::Local: return "local";
        case StorageUsbState::Preparing: return "preparing";
        case StorageUsbState::Shared: return "shared";
        case StorageUsbState::Returning: return "returning";
        case StorageUsbState::Restoring: return "restoring";
        case StorageUsbState::Error: return "error";
    }
    return "error";
}

#if AC_USB_SD_SUPPORTED
// All USB transfers finish synchronously under the media lock. Returning the
// card to FAT cannot race an in-flight sector write. The host must flush its
// own filesystem cache before requesting eject or manual return.
int32_t transfer(uint32_t lba, uint32_t offset, void *buffer,
                 uint32_t length, bool write) {
    if (!media_mutex) return -1;
    xSemaphoreTake(media_mutex, portMAX_DELAY);

    const uint64_t position = uint64_t(lba) * SectorBytes + offset;
    const uint64_t capacity = uint64_t(card.csd.capacity) * SectorBytes;
    if (state.load() != StorageUsbState::Shared || position > capacity ||
        length > capacity - position) {
        xSemaphoreGive(media_mutex);
        tud_msc_set_sense(0, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return -1;
    }

    auto *bytes = static_cast<uint8_t *>(buffer);
    size_t done = 0;
    esp_err_t error = ESP_OK;
    while (done < length && error == ESP_OK) {
        const uint64_t at = position + done;
        const size_t sector = at / SectorBytes;
        const size_t skip = at % SectorBytes;
        size_t amount = std::min<size_t>(length - done, TransferBytes - skip);
        const size_t count = (skip + amount + SectorBytes - 1) / SectorBytes;

        if (!write || skip || amount % SectorBytes) {
            error = sdmmc_read_sectors(&card, sector_buffer, sector, count);
        }
        if (error != ESP_OK) break;

        if (write) {
            memcpy(sector_buffer + skip, bytes + done, amount);
            error = sdmmc_write_sectors(&card, sector_buffer, sector, count);
        } else {
            memcpy(bytes + done, sector_buffer + skip, amount);
        }
        done += amount;
    }

    xSemaphoreGive(media_mutex);
    if (error != ESP_OK) {
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[USB] %s failed lba=%lu error=%s",
                  write ? "write" : "read", static_cast<unsigned long>(lba),
                  esp_err_to_name(error));
        tud_msc_set_sense(0, SCSI_SENSE_MEDIUM_ERROR, write ? 0x0c : 0x11, 0);
        return -1;
    }
    return static_cast<int32_t>(length);
}
#endif

}  // namespace aircannect::StorageUsb

#if AC_USB_SD_SUPPORTED
using namespace aircannect;

extern "C" {
uint8_t tud_msc_get_maxlun_cb() { return 1; }

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t product[16],
                        uint8_t revision[4]) {
    memcpy(vendor, "AirCAN  ", 8);
    memcpy(product, "AirCANnect SD   ", 16);
    memcpy(revision, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t) {
    if (StorageUsb::state.load() == StorageUsbState::Shared) return true;

    tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3a, 0);
    return false;
}

void tud_msc_capacity_cb(uint8_t, uint32_t *count, uint16_t *size) {
    *count = 0;
    *size = 512;
    if (!StorageUsb::media_mutex) return;

    xSemaphoreTake(StorageUsb::media_mutex, portMAX_DELAY);
    if (StorageUsb::state.load() == StorageUsbState::Shared) {
        *count = StorageUsb::card.csd.capacity;
    }
    xSemaphoreGive(StorageUsb::media_mutex);
}

bool tud_msc_is_writable_cb(uint8_t) { return true; }

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool eject) {
    if (!eject || start) return true;
    if (!StorageUsb::mutex) return false;

    xSemaphoreTake(StorageUsb::media_mutex, portMAX_DELAY);
    const bool allowed = !StorageUsb::prevent_removal;
    xSemaphoreTake(StorageUsb::mutex, portMAX_DELAY);
    if (allowed && StorageUsb::snapshot.state == StorageUsbState::Shared) {
        StorageUsb::publish_locked(StorageUsbState::Returning);
    }
    xSemaphoreGive(StorageUsb::mutex);
    xSemaphoreGive(StorageUsb::media_mutex);
    if (allowed && StorageUsb::wake) StorageUsb::wake();
    if (!allowed) tud_msc_set_sense(0, SCSI_SENSE_ILLEGAL_REQUEST, 0x53, 2);
    return allowed;
}

int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset,
                         void *buffer, uint32_t length) {
    return StorageUsb::transfer(lba, offset, buffer, length, false);
}

int32_t tud_msc_write10_cb(uint8_t, uint32_t lba, uint32_t offset,
                          uint8_t *buffer, uint32_t length) {
    return StorageUsb::transfer(lba, offset, buffer, length, true);
}

int32_t tud_msc_scsi_cb(uint8_t, const uint8_t command[16], void *, uint16_t) {
    if (!tud_msc_test_unit_ready_cb(0)) return -1;

    // Writes are synchronous, so SYNCHRONIZE CACHE has nothing left to flush.
    if (command[0] == 0x35) return 0;
    if (command[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) {
        xSemaphoreTake(StorageUsb::media_mutex, portMAX_DELAY);
        StorageUsb::prevent_removal = (command[4] & 1) != 0;
        xSemaphoreGive(StorageUsb::media_mutex);
        return 0;
    }

    tud_msc_set_sense(0, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return -1;
}
}
#endif
