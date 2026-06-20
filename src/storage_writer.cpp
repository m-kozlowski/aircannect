#include "storage_writer.h"

#include <FS.h>
#include <stdio.h>
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
    bool rotating = false;
    size_t rotate_bytes = 0;
    uint8_t archive_count = 0;
};

StorageWriterStatus stats;
Slot *slots = nullptr;
uint8_t *data_pool = nullptr;
size_t capacity = 0;
size_t chunk_bytes = AC_STORAGE_WRITE_CHUNK_BYTES;
size_t head = 0;
size_t tail = 0;
size_t queued = 0;
char rotate_cached_path[AC_STORAGE_WRITE_PATH_MAX] = {};
size_t rotate_cached_limit = 0;
uint8_t rotate_cached_archives = 0;
uint64_t rotate_cached_size = 0;
bool rotate_cached_valid = false;

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
        slots[i].rotating = false;
        slots[i].rotate_bytes = 0;
        slots[i].archive_count = 0;
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
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[WRITER] allocation failed chunk=%u\n",
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
    slot.rotating = false;
    slot.rotate_bytes = 0;
    slot.archive_count = 0;
    tail = (tail + 1) % capacity;
    queued++;
    return true;
}

bool push_rotating_chunk(const char *path,
                         const uint8_t *data,
                         size_t len,
                         size_t rotate_bytes,
                         uint8_t archive_count) {
    if (!push_chunk(path, data, len)) return false;
    const size_t index = (tail + capacity - 1) % capacity;
    slots[index].rotating = true;
    slots[index].rotate_bytes = rotate_bytes;
    slots[index].archive_count = archive_count;
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
    src.rotating = false;
    src.rotate_bytes = 0;
    src.archive_count = 0;
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
    Log::logf(CAT_STORAGE, LOG_WARN,
              "[WRITER] dropped %u queued chunks: storage not mounted\n",
              static_cast<unsigned>(dropped));
}

bool format_archive_path(const char *base,
                         uint8_t index,
                         char *out,
                         size_t out_size) {
    if (!base || !out || out_size == 0 || index == 0) return false;
    const int len = snprintf(out, out_size, "%s.%u", base,
                             static_cast<unsigned>(index));
    return len > 0 && len < static_cast<int>(out_size);
}

bool remove_if_exists(fs::FS &fs, const char *path) {
    return path && (!fs.exists(path) || fs.remove(path));
}

bool rotate_cache_matches(const Slot &slot) {
    return rotate_cached_valid &&
           rotate_cached_limit == slot.rotate_bytes &&
           rotate_cached_archives == slot.archive_count &&
           strncmp(rotate_cached_path, slot.path,
                   sizeof(rotate_cached_path)) == 0;
}

uint64_t current_rotating_size(fs::FS &fs, const Slot &slot) {
    if (rotate_cache_matches(slot)) return rotate_cached_size;

    uint64_t size = 0;
    if (fs.exists(slot.path)) {
        File file = fs.open(slot.path, "r");
        if (file) {
            size = file.size();
            file.close();
        }
    }

    copy_cstr(rotate_cached_path, sizeof(rotate_cached_path), slot.path);
    rotate_cached_limit = slot.rotate_bytes;
    rotate_cached_archives = slot.archive_count;
    rotate_cached_size = size;
    rotate_cached_valid = true;
    return size;
}

void invalidate_rotate_cache_for_path(const char *path) {
    if (rotate_cached_valid &&
        strncmp(rotate_cached_path, path, sizeof(rotate_cached_path)) == 0) {
        rotate_cached_valid = false;
        rotate_cached_path[0] = 0;
        rotate_cached_size = 0;
    }
}

bool rotate_file(fs::FS &fs, const Slot &slot) {
    if (!slot.archive_count) {
        if (!remove_if_exists(fs, slot.path)) return false;
        stats.rotations++;
        copy_cstr(rotate_cached_path, sizeof(rotate_cached_path), slot.path);
        rotate_cached_limit = slot.rotate_bytes;
        rotate_cached_archives = slot.archive_count;
        rotate_cached_size = 0;
        rotate_cached_valid = true;
        return true;
    }

    char src[AC_STORAGE_WRITE_PATH_MAX + 8] = {};
    char dst[AC_STORAGE_WRITE_PATH_MAX + 8] = {};

    if (format_archive_path(slot.path, slot.archive_count,
                            dst, sizeof(dst))) {
        if (!remove_if_exists(fs, dst)) return false;
    }

    for (int i = static_cast<int>(slot.archive_count); i >= 2; --i) {
        if (!format_archive_path(slot.path, static_cast<uint8_t>(i - 1),
                                 src, sizeof(src)) ||
            !format_archive_path(slot.path, static_cast<uint8_t>(i),
                                 dst, sizeof(dst))) {
            return false;
        }
        if (!fs.exists(src)) continue;
        if (!remove_if_exists(fs, dst)) return false;
        if (!fs.rename(src, dst)) return false;
    }

    if (fs.exists(slot.path)) {
        if (!format_archive_path(slot.path, 1, dst, sizeof(dst))) {
            return false;
        }
        if (!remove_if_exists(fs, dst)) return false;
        if (!fs.rename(slot.path, dst)) return false;
    }

    stats.rotations++;
    copy_cstr(rotate_cached_path, sizeof(rotate_cached_path), slot.path);
    rotate_cached_limit = slot.rotate_bytes;
    rotate_cached_archives = slot.archive_count;
    rotate_cached_size = 0;
    rotate_cached_valid = true;
    return true;
}

bool rotate_if_needed(fs::FS &fs, const Slot &slot) {
    if (!slot.rotating || slot.rotate_bytes == 0) return true;
    const uint64_t current_size = current_rotating_size(fs, slot);
    if (current_size + slot.len <= slot.rotate_bytes) return true;
    return rotate_file(fs, slot);
}

void note_write_complete(const Slot &slot, size_t written) {
    if (slot.rotating && rotate_cache_matches(slot)) {
        rotate_cached_size += written;
    } else if (!slot.rotating) {
        invalidate_rotate_cache_for_path(slot.path);
    }
}

bool write_chunk(const Slot &slot) {
    Storage::Guard g;
    const StorageStatus storage = Storage::status();
    if (!storage.mounted) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        return false;
    }

    fs::FS *fs = nullptr;
    switch (storage.type) {
        case StorageType::SdMmc:
#if AC_STORAGE_SDMMC_ENABLED && SOC_SDMMC_HOST_SUPPORTED
            fs = &SD_MMC;
#endif
            break;
        case StorageType::SpiSd:
#if AC_STORAGE_SPI_SD_ENABLED
            fs = &SD;
#endif
            break;
        case StorageType::None:
        default:
            break;
    }

    if (slot.rotating) {
        if (!fs || !rotate_if_needed(*fs, slot)) {
            stats.rotate_errors++;
            set_error("rotate_failed");
            return false;
        }
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
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[WRITER] open failed path=%s type=%s\n",
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
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[WRITER] short write path=%s written=%u expected=%u\n",
                  slot.path,
                  static_cast<unsigned>(written),
                  static_cast<unsigned>(slot.len));
        return false;
    }

    stats.written++;
    stats.bytes_written += written;
    note_write_complete(slot, written);
    stats.last_error[0] = 0;
    return true;
}

bool enqueue_append_internal(const char *path,
                             const uint8_t *data,
                             size_t len,
                             bool rotating,
                             size_t rotate_bytes,
                             uint8_t archive_count,
                             bool report_errors) {
    if (!stats.initialized) begin();
    if (!stats.available || !data || len == 0) return false;
    if (!valid_path(path)) {
        stats.queue_drops++;
        set_error("bad_path");
        if (report_errors) {
            Log::logf(CAT_STORAGE, LOG_WARN,
                      "[WRITER] rejected write: bad path\n");
        }
        return false;
    }
    if (!Storage::mounted()) {
        stats.unavailable_drops++;
        set_error("storage_not_mounted");
        if (report_errors) {
            Log::logf(CAT_STORAGE, LOG_WARN,
                      "[WRITER] rejected write path=%s: storage not mounted\n",
                      path);
        }
        return false;
    }

    const size_t needed = (len + chunk_bytes - 1) / chunk_bytes;
    if (needed == 0 || needed > free_slots()) {
        stats.queue_drops++;
        set_error("queue_full");
        if (report_errors) {
            Log::logf(CAT_STORAGE, LOG_WARN,
                      "[WRITER] rejected write path=%s chunks=%u free=%u\n",
                      path,
                      static_cast<unsigned>(needed),
                      static_cast<unsigned>(free_slots()));
        }
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t part = len - offset;
        if (part > chunk_bytes) part = chunk_bytes;
        const bool pushed =
            rotating
                ? push_rotating_chunk(path,
                                      data + offset,
                                      part,
                                      rotate_bytes,
                                      archive_count)
                : push_chunk(path, data + offset, part);
        if (!pushed) {
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

}  // namespace

void poll_with_budget(size_t max_items, size_t max_bytes) {
    if (!stats.initialized) begin();
    if (!stats.available || !queued || max_items == 0 || max_bytes == 0) {
        return;
    }
    if (!Storage::mounted()) {
        drop_all_unavailable();
        return;
    }

    size_t items = 0;
    size_t bytes = 0;
    while (queued && items < max_items && bytes < max_bytes) {
        Slot slot;
        if (!pop_chunk(slot)) break;
        bytes += slot.len;
        write_chunk(slot);
        items++;
    }
}

void begin() {
    if (stats.initialized) return;
    stats.initialized = true;
    stats.chunk_bytes = chunk_bytes;
    stats.available = allocate_buffers();
    stats.capacity = capacity;
    reset_queue();
    if (stats.available) {
        Log::logf(CAT_STORAGE, LOG_DEBUG,
                  "[WRITER] ready q=%u chunk=%u psram=%s\n",
                  static_cast<unsigned>(capacity),
                  static_cast<unsigned>(chunk_bytes),
                  stats.using_psram ? "yes" : "no");
    }
}

void poll() {
    poll_with_budget(AC_STORAGE_WRITE_BUDGET_ITEMS,
                     AC_STORAGE_WRITE_BUDGET_BYTES);
}

void poll_limited(size_t max_items, size_t max_bytes) {
    poll_with_budget(max_items, max_bytes);
}

bool enqueue_append(const char *path, const uint8_t *data, size_t len) {
    return enqueue_append_internal(path, data, len, false, 0, 0, true);
}

bool enqueue_rotating_append(const char *path,
                             const uint8_t *data,
                             size_t len,
                             size_t rotate_bytes,
                             uint8_t archive_count,
                             bool report_errors) {
    return enqueue_append_internal(path,
                                   data,
                                   len,
                                   rotate_bytes > 0,
                                   rotate_bytes,
                                   archive_count,
                                   report_errors);
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
