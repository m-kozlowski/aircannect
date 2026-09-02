#include "crash_diagnostics.h"

#include <Arduino.h>
#include <esp32-hal.h>
#include <esp_core_dump.h>
#include <esp_err.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <sdkconfig.h>

#include <algorithm>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "large_byte_buffer.h"
#include "version.h"

namespace aircannect {
namespace {

static constexpr uint32_t RTC_PANIC_MAGIC = 0x43524143u;
static constexpr uint16_t RTC_PANIC_SCHEMA = 1;
static constexpr uint32_t RTC_PANIC_CAPTURED = 1u << 0;
static constexpr uint32_t RTC_BACKTRACE_CORRUPT = 1u << 1;
static constexpr uint32_t RTC_BACKTRACE_CONTINUES = 1u << 2;

struct RtcPanicRecord {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t checksum;
    uint32_t flags;
    int32_t core;
    uint32_t pc;
    uint32_t backtrace_depth;
    uint32_t backtrace[AC_CRASH_BACKTRACE_MAX];
    char firmware[AC_CRASH_FIRMWARE_VERSION_MAX];
};

RTC_NOINIT_ATTR RtcPanicRecord rtc_panic_record;

uint32_t IRAM_ATTR rtc_record_checksum(RtcPanicRecord &record) {
    const uint32_t saved = record.checksum;
    record.checksum = 0;

    const size_t offset = offsetof(RtcPanicRecord, schema);
    const uint32_t checksum = esp_rom_crc32_le(
        0,
        reinterpret_cast<const uint8_t *>(&record) + offset,
        sizeof(record) - offset);
    record.checksum = saved;
    return checksum;
}

bool rtc_record_valid(RtcPanicRecord record) {
    if (record.magic != RTC_PANIC_MAGIC ||
        record.schema != RTC_PANIC_SCHEMA ||
        record.size != sizeof(record)) {
        return false;
    }

    return record.checksum == rtc_record_checksum(record);
}

bool crash_reset_reason(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_CPU_LOCKUP:
            return true;
        default:
            return false;
    }
}

void prepare_rtc_record() {
    rtc_panic_record.magic = 0;
    memset(&rtc_panic_record, 0, sizeof(rtc_panic_record));
    rtc_panic_record.schema = RTC_PANIC_SCHEMA;
    rtc_panic_record.size = sizeof(rtc_panic_record);
    rtc_panic_record.core = -1;
    strncpy(rtc_panic_record.firmware, aircannect_version(),
            sizeof(rtc_panic_record.firmware) - 1);
    rtc_panic_record.checksum = rtc_record_checksum(rtc_panic_record);

    __asm__ __volatile__("memw" ::: "memory");
    rtc_panic_record.magic = RTC_PANIC_MAGIC;
}

void IRAM_ATTR capture_rtc_panic(arduino_panic_info_t *info, void *) {
    if (!info) return;

    rtc_panic_record.magic = 0;
    rtc_panic_record.flags = RTC_PANIC_CAPTURED;
    if (info->backtrace_corrupt) {
        rtc_panic_record.flags |= RTC_BACKTRACE_CORRUPT;
    }
    if (info->backtrace_continues) {
        rtc_panic_record.flags |= RTC_BACKTRACE_CONTINUES;
    }
    rtc_panic_record.core = info->core;
    rtc_panic_record.pc = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(info->pc));

    const size_t available_depth =
        static_cast<size_t>(info->backtrace_len);
    const size_t depth = available_depth < AC_CRASH_BACKTRACE_MAX
                             ? available_depth
                             : AC_CRASH_BACKTRACE_MAX;
    rtc_panic_record.backtrace_depth = depth;
    for (size_t i = 0; i < depth; ++i) {
        rtc_panic_record.backtrace[i] = info->backtrace[i];
    }
    for (size_t i = depth; i < AC_CRASH_BACKTRACE_MAX; ++i) {
        rtc_panic_record.backtrace[i] = 0;
    }

    rtc_panic_record.checksum = rtc_record_checksum(rtc_panic_record);
    __asm__ __volatile__("memw" ::: "memory");
    rtc_panic_record.magic = RTC_PANIC_MAGIC;
}

void copy_text(char *out, size_t out_size, const char *text) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!text) return;

    strncpy(out, text, out_size - 1);
    out[out_size - 1] = 0;
}

void copy_error(char *out, size_t out_size, esp_err_t error) {
    copy_text(out, out_size, esp_err_to_name(error));
}

void import_rtc_snapshot(CrashDiagnosticsSnapshot &out) {
    const RtcPanicRecord retained = rtc_panic_record;
    if (!rtc_record_valid(retained) ||
        !(retained.flags & RTC_PANIC_CAPTURED) ||
        !crash_reset_reason(esp_reset_reason())) {
        return;
    }

    out.rtc_panic_available = true;
    copy_text(out.rtc_firmware, sizeof(out.rtc_firmware), retained.firmware);
    out.rtc_core = static_cast<int8_t>(retained.core);
    out.rtc_pc = retained.pc;
    out.rtc_backtrace_depth = static_cast<uint8_t>(std::min(
        static_cast<size_t>(retained.backtrace_depth),
        AC_CRASH_BACKTRACE_MAX));
    memcpy(out.rtc_backtrace, retained.backtrace,
           out.rtc_backtrace_depth * sizeof(out.rtc_backtrace[0]));
    out.rtc_backtrace_corrupt =
        retained.flags & RTC_BACKTRACE_CORRUPT;
    out.rtc_backtrace_continues =
        retained.flags & RTC_BACKTRACE_CONTINUES;
}

}  // namespace

const char *crash_dump_state_name(CrashDumpState state) {
    switch (state) {
        case CrashDumpState::Unsupported: return "unsupported";
        case CrashDumpState::Empty: return "empty";
        case CrashDumpState::Available: return "available";
        case CrashDumpState::Invalid: return "invalid";
    }
    return "unknown";
}

bool CrashDiagnostics::begin() {
    import_rtc_snapshot(snapshot_);
    prepare_rtc_record();
    set_arduino_panic_handler(capture_rtc_panic, nullptr);

    if (!mutex_) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }
    if (!mutex_) return false;

    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    refresh_dump_locked();
    xSemaphoreGive(mutex_);
    return true;
}

void CrashDiagnostics::refresh_dump_locked() {
    const bool rtc_available = snapshot_.rtc_panic_available;
    char rtc_firmware[sizeof(snapshot_.rtc_firmware)] = {};
    uint32_t rtc_backtrace[AC_CRASH_BACKTRACE_MAX] = {};
    const int8_t rtc_core = snapshot_.rtc_core;
    const uint32_t rtc_pc = snapshot_.rtc_pc;
    const uint8_t rtc_depth = snapshot_.rtc_backtrace_depth;
    const bool rtc_corrupt = snapshot_.rtc_backtrace_corrupt;
    const bool rtc_continues = snapshot_.rtc_backtrace_continues;
    memcpy(rtc_firmware, snapshot_.rtc_firmware, sizeof(rtc_firmware));
    memcpy(rtc_backtrace, snapshot_.rtc_backtrace, sizeof(rtc_backtrace));

    snapshot_ = {};
    snapshot_.rtc_panic_available = rtc_available;
    memcpy(snapshot_.rtc_firmware, rtc_firmware, sizeof(rtc_firmware));
    snapshot_.rtc_core = rtc_core;
    snapshot_.rtc_pc = rtc_pc;
    snapshot_.rtc_backtrace_depth = rtc_depth;
    memcpy(snapshot_.rtc_backtrace, rtc_backtrace, sizeof(rtc_backtrace));
    snapshot_.rtc_backtrace_corrupt = rtc_corrupt;
    snapshot_.rtc_backtrace_continues = rtc_continues;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        nullptr);
    partition_ = partition;
    if (!partition) {
        snapshot_.dump_state = CrashDumpState::Unsupported;
        return;
    }

    uint32_t stored_size = 0;
    const esp_err_t header_read = esp_partition_read(
        partition, 0, &stored_size, sizeof(stored_size));
    if (header_read != ESP_OK) {
        snapshot_.dump_state = CrashDumpState::Invalid;
        copy_error(snapshot_.dump_error, sizeof(snapshot_.dump_error),
                   header_read);
        return;
    }
    if (stored_size == UINT32_MAX) {
        snapshot_.dump_state = CrashDumpState::Empty;
        return;
    }

    const esp_err_t check = esp_core_dump_image_check();
    if (check == ESP_ERR_NOT_FOUND) {
        snapshot_.dump_state = CrashDumpState::Empty;
        return;
    }
    if (check != ESP_OK) {
        const bool corrupt_image =
            check == ESP_ERR_INVALID_SIZE || check == ESP_ERR_INVALID_CRC;
        const bool follows_crash =
            crash_reset_reason(esp_reset_reason()) || rtc_available;
        if (corrupt_image && !follows_crash) {
            snapshot_.dump_state = CrashDumpState::Empty;
            return;
        }

        snapshot_.dump_state = CrashDumpState::Invalid;
        copy_error(snapshot_.dump_error, sizeof(snapshot_.dump_error), check);
        return;
    }

    size_t address = 0;
    size_t size = 0;
    const esp_err_t image = esp_core_dump_image_get(&address, &size);
    if (image != ESP_OK || address != partition->address ||
        size > partition->size) {
        snapshot_.dump_state = CrashDumpState::Invalid;
        copy_error(snapshot_.dump_error, sizeof(snapshot_.dump_error),
                   image == ESP_OK ? ESP_ERR_INVALID_SIZE : image);
        return;
    }

    snapshot_.dump_state = CrashDumpState::Available;
    snapshot_.dump_size = size;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t summary = {};
    const esp_err_t summary_result = esp_core_dump_get_summary(&summary);
    if (summary_result != ESP_OK) {
        copy_error(snapshot_.dump_error, sizeof(snapshot_.dump_error),
                   summary_result);
        return;
    }

    snapshot_.summary_available = true;
    memcpy(snapshot_.task, summary.exc_task,
           std::min(sizeof(summary.exc_task), sizeof(snapshot_.task) - 1));
    snapshot_.exception_pc = summary.exc_pc;
    snapshot_.exception_cause = summary.ex_info.exc_cause;
    snapshot_.exception_address = summary.ex_info.exc_vaddr;
    snapshot_.backtrace_depth = static_cast<uint8_t>(std::min(
        static_cast<size_t>(summary.exc_bt_info.depth),
        AC_CRASH_BACKTRACE_MAX));
    memcpy(snapshot_.backtrace, summary.exc_bt_info.bt,
           snapshot_.backtrace_depth * sizeof(snapshot_.backtrace[0]));
    snapshot_.backtrace_corrupt = summary.exc_bt_info.corrupted;
    memcpy(snapshot_.elf_sha, summary.app_elf_sha256,
           std::min(sizeof(summary.app_elf_sha256),
                    sizeof(snapshot_.elf_sha) - 1));

    const esp_err_t reason = esp_core_dump_get_panic_reason(
        snapshot_.reason, sizeof(snapshot_.reason));
    if (reason != ESP_OK && reason != ESP_ERR_NOT_FOUND) {
        copy_error(snapshot_.dump_error, sizeof(snapshot_.dump_error), reason);
    }
#endif
}

bool CrashDiagnostics::copy_snapshot(CrashDiagnosticsSnapshot &out) const {
    if (!mutex_ ||
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    out = snapshot_;
    xSemaphoreGive(mutex_);
    return true;
}

std::shared_ptr<const LargeByteBuffer> CrashDiagnostics::copy_dump(
    char *error,
    size_t error_size) const {
    if (error && error_size) error[0] = 0;
    if (!mutex_ ||
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        copy_text(error, error_size, "busy");
        return {};
    }

    if (snapshot_.dump_state != CrashDumpState::Available ||
        !partition_ || snapshot_.dump_size == 0) {
        copy_text(error, error_size,
                  crash_dump_state_name(snapshot_.dump_state));
        xSemaphoreGive(mutex_);
        return {};
    }

    std::unique_ptr<LargeByteBuffer> buffer =
        LargeByteBuffer::allocate(snapshot_.dump_size);
    if (!buffer) {
        copy_text(error, error_size, "allocation_failed");
        xSemaphoreGive(mutex_);
        return {};
    }

    const esp_err_t read = esp_partition_read(
        static_cast<const esp_partition_t *>(partition_), 0,
        buffer->data(), buffer->size());
    xSemaphoreGive(mutex_);
    if (read != ESP_OK) {
        copy_error(error, error_size, read);
        return {};
    }

    return LargeByteBuffer::freeze(std::move(buffer));
}

bool CrashDiagnostics::clear(char *error, size_t error_size) {
    if (error && error_size) error[0] = 0;
    if (!mutex_ ||
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        copy_text(error, error_size, "busy");
        return false;
    }

    if (!partition_) {
        if (snapshot_.rtc_panic_available) {
            snapshot_.rtc_panic_available = false;
            xSemaphoreGive(mutex_);
            return true;
        }

        copy_text(error, error_size, "unsupported");
        xSemaphoreGive(mutex_);
        return false;
    }

    const esp_err_t result = esp_core_dump_image_erase();
    if (result != ESP_OK) {
        copy_error(error, error_size, result);
        xSemaphoreGive(mutex_);
        return false;
    }

    snapshot_.rtc_panic_available = false;
    refresh_dump_locked();
    xSemaphoreGive(mutex_);
    return true;
}

void CrashDiagnostics::log_previous_crash() const {
    CrashDiagnosticsSnapshot snapshot;
    if (!copy_snapshot(snapshot)) return;

    if (snapshot.dump_state == CrashDumpState::Available) {
        Log::logf(
            CAT_GENERAL, LOG_WARN,
            "[CRASH] previous panic dump available task=%s reason=%s "
            "pc=0x%08lx bytes=%u elf_sha=%s rtc=%s\n",
            snapshot.task[0] ? snapshot.task : "--",
            snapshot.reason[0] ? snapshot.reason : "--",
            static_cast<unsigned long>(snapshot.exception_pc),
            static_cast<unsigned>(snapshot.dump_size),
            snapshot.elf_sha[0] ? snapshot.elf_sha : "--",
            snapshot.rtc_panic_available ? "yes" : "no");
        return;
    }

    if (snapshot.dump_state == CrashDumpState::Invalid) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[CRASH] retained panic dump invalid error=%s rtc=%s\n",
                  snapshot.dump_error[0] ? snapshot.dump_error : "unknown",
                  snapshot.rtc_panic_available ? "yes" : "no");
        return;
    }

    if (snapshot.rtc_panic_available) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[CRASH] previous panic breadcrumb available firmware=%s "
                  "core=%d pc=0x%08lx backtrace_depth=%u\n",
                  snapshot.rtc_firmware[0] ? snapshot.rtc_firmware : "--",
                  static_cast<int>(snapshot.rtc_core),
                  static_cast<unsigned long>(snapshot.rtc_pc),
                  static_cast<unsigned>(snapshot.rtc_backtrace_depth));
    }
}

}  // namespace aircannect
