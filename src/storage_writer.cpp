#include "storage_writer.h"

#include <FS.h>
#include <string.h>

#include "board.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

#if AC_STORAGE_SDMMC_ENABLED
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include <SD_MMC.h>
#endif
#endif

#if AC_STORAGE_SPI_SD_ENABLED
#include <SD.h>
#endif

namespace aircannect {
namespace StorageWriter {
namespace {

struct Slot {
    char path[AC_STORAGE_WRITE_PATH_MAX] = {};
    uint8_t *data = nullptr;
    size_t len = 0;
};

StorageWriterStatus stats;
Slot *slots = nullptr;
uint8_t *data_pool = nullptr;
size_t capacity = 0;
size_t chunk_bytes = AC_STORAGE_WRITE_CHUNK_BYTES;
size_t head = 0;
size_t tail = 0;
size_t queued = 0;

void set_error(const char *error) {
    copy_cstr(stats.last_error, sizeof(stats.last_error), error);
    stats.last_activity_ms = millis();
}

void reset_queue() {
    head = 0;
    tail = 0;
    queued = 0;
    if (!slots) return;
    for (size_t i = 0; i < capacity; ++i) {
        slots[i].path[0] = 0;
        slots[i].len = 0;
    }
}

bool valid_path(const char *path) {
    if (!path || path[0] != '/') return false;
    const size_t len = strlen(path);
    return len > 1 && len < AC_STORAGE_WRITE_PATH_MAX;
}

size_t free_slots() {
    return capacity > queued ? capacity - queued : 0;
}

bool allocate_buffers() {
    const bool want_psram = Memory::psram_available();
    capacity = want_psram ? AC_STORAGE_WRITE_QUEUE_PSRAM
                          : AC_STORAGE_WRITE_QUEUE_INTERNAL;
    if (capacity == 0 || chunk_bytes == 0) return false;

    slots = static_cast<Slot *>(Memory::alloc_internal(sizeof(Slot) *
                                                       capacity));
    data_pool = static_cast<uint8_t *>(Memory::alloc_large(chunk_bytes *
                                                           capacity,
                                                           !want_psram));
    if (!data_pool && want_psram) {
        Memory::free(slots);
        capacity = AC_STORAGE_WRITE_QUEUE_INTERNAL;
        slots = static_cast<Slot *>(Memory::alloc_internal(sizeof(Slot) *
                                                           capacity));
        data_pool = static_cast<uint8_t *>(Memory::alloc_large(chunk_bytes *
                                                               capacity,
                                                               true));
        stats.using_psram = false;
    } else {
        stats.using_psram = want_psram;
    }
    if (!slots || !data_pool) {
        Memory::free(slots);
        Memory::free(data_pool);
        slots = nullptr;
        data_pool = nullptr;
        capacity = 0;
        set_error("allocation_failed");
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[STORAGE_WRITER] allocation failed chunk=%u\n",
                  static_cast<unsigned>(chunk_bytes));
        return false;
    }

    memset(slots, 0, sizeof(Slot) * capacity);
    for (size_t i = 0; i < capacity; ++i) {
        slots[i].data = data_pool + i * chunk_bytes;
    }
    return true;
}

bool push_chunk(const char *path, const uint8_t *data, size_t len) {
    if (queued >= capacity || !slots || !data || len == 0 ||
        len > chunk_bytes) {
        return false;
    }

    Slot &slot = slots[tail];
    copy_cstr(slot.path, sizeof(slot.path), path);
    memcpy(slot.data, data, len);
    slot.len = len;
    tail = (tail + 1) % capacity;
    queued++;
    return true;
}

bool pop_chunk(Slot &slot) {
    if (!queued || !slots) return false;
    Slot &src = slots[head];
    copy_cstr(slot.path, sizeof(slot.path), src.path);
    slot.data = src.data;
    slot.len = src.len;
    src.path[0] = 0;
    src.len = 0;
    head = (head + 1) % capacity;
    queued--;
    return true;
}

void drop_all_unavailable() {
    if (!queued) return;
    const size_t dropped = queued;
    stats.unavailable_drops += static_cast<uint32_t>(queued);
    reset_queue();
    set_error("storage_not_mounted");
    Log::logf(CAT_GENERAL, LOG_WARN,
              "[STORAGE_WRITER] dropped %u queued chunks: storage not mounted\n",
              static_cast<unsigned>(dropped));
}

bool write_chunk(const Slot &slot) {
    Storage::Guard g;
    const StorageStatus storage = Storage::status();
    if (!storage.mounted) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        return false;
    }

    File file;
    switch (storage.type) {
        case StorageType::SdMmc:
#if AC_STORAGE_SDMMC_ENABLED && SOC_SDMMC_HOST_SUPPORTED
            file = SD_MMC.open(slot.path, FILE_APPEND);
#else
            set_error("sdmmc_not_available");
            stats.open_errors++;
            return false;
#endif
            break;
        case StorageType::SpiSd:
#if AC_STORAGE_SPI_SD_ENABLED
            file = SD.open(slot.path, FILE_APPEND);
#else
            set_error("spi_sd_not_available");
            stats.open_errors++;
            return false;
#endif
            break;
        case StorageType::None:
        default:
            stats.unavailable_drops++;
            set_error("storage_not_configured");
            return false;
    }

    if (!file) {
        stats.open_errors++;
        set_error("open_failed");
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[STORAGE_WRITER] open failed path=%s type=%s\n",
                  slot.path, Storage::type_name(storage.type));
        return false;
    }

    const size_t written = file.write(slot.data, slot.len);
    file.close();
    copy_cstr(stats.last_path, sizeof(stats.last_path), slot.path);
    stats.last_activity_ms = millis();
    if (written != slot.len) {
        stats.write_errors++;
        set_error("short_write");
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[STORAGE_WRITER] short write path=%s written=%u expected=%u\n",
                  slot.path,
                  static_cast<unsigned>(written),
                  static_cast<unsigned>(slot.len));
        return false;
    }

    stats.written++;
    stats.bytes_written += written;
    stats.last_error[0] = 0;
    return true;
}

}  // namespace

void begin() {
    if (stats.initialized) return;
    stats.initialized = true;
    stats.chunk_bytes = chunk_bytes;
    stats.available = allocate_buffers();
    stats.capacity = capacity;
    reset_queue();
    if (stats.available) {
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[STORAGE_WRITER] ready q=%u chunk=%u psram=%s\n",
                  static_cast<unsigned>(capacity),
                  static_cast<unsigned>(chunk_bytes),
                  stats.using_psram ? "yes" : "no");
    }
}

void poll() {
    if (!stats.initialized) begin();
    if (!stats.available || !queued) return;
    if (!Storage::mounted()) {
        drop_all_unavailable();
        return;
    }

    size_t items = 0;
    size_t bytes = 0;
    while (queued && items < AC_STORAGE_WRITE_BUDGET_ITEMS &&
           bytes < AC_STORAGE_WRITE_BUDGET_BYTES) {
        Slot slot;
        if (!pop_chunk(slot)) break;
        bytes += slot.len;
        write_chunk(slot);
        items++;
    }
}

bool enqueue_append(const char *path, const uint8_t *data, size_t len) {
    if (!stats.initialized) begin();
    if (!stats.available || !data || len == 0) return false;
    if (!valid_path(path)) {
        stats.queue_drops++;
        set_error("bad_path");
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[STORAGE_WRITER] rejected write: bad path\n");
        return false;
    }
    if (!Storage::mounted()) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[STORAGE_WRITER] rejected write path=%s: storage not mounted\n",
                  path);
        return false;
    }

    const size_t needed = (len + chunk_bytes - 1) / chunk_bytes;
    if (needed == 0 || needed > free_slots()) {
        stats.queue_drops++;
        set_error("queue_full");
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[STORAGE_WRITER] rejected write path=%s chunks=%u free=%u\n",
                  path,
                  static_cast<unsigned>(needed),
                  static_cast<unsigned>(free_slots()));
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t part = len - offset;
        if (part > chunk_bytes) part = chunk_bytes;
        if (!push_chunk(path, data + offset, part)) {
            stats.queue_drops++;
            set_error("queue_full");
            return false;
        }
        offset += part;
    }

    stats.enqueued += static_cast<uint32_t>(needed);
    stats.bytes_enqueued += len;
    stats.queued = queued;
    stats.last_activity_ms = millis();
    copy_cstr(stats.last_path, sizeof(stats.last_path), path);
    return true;
}

StorageWriterStatus status() {
    if (!stats.initialized) begin();
    StorageWriterStatus out = stats;
    out.capacity = capacity;
    out.queued = queued;
    out.chunk_bytes = chunk_bytes;
    return out;
}

}  // namespace StorageWriter
}  // namespace aircannect
