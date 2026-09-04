#include "crash_diagnostics.h"

#include <Arduino.h>
#include <esp32-hal.h>
#include <esp_core_dump.h>
#include <esp_err.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <sdkconfig.h>

#include <algorithm>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
static constexpr uint32_t RTC_PANIC_TASK_WDT = 1u << 3;
static constexpr uint32_t RTC_DUMP_RELATION_KNOWN = 1u << 4;
static constexpr uint32_t RTC_DUMP_RELATION_CURRENT = 1u << 5;
static constexpr uint32_t RTC_ARM_MAGIC = 0x41524343u;
static constexpr uint16_t RTC_ARM_SCHEMA = 1;
static constexpr uint32_t RTC_ARM_FINGERPRINT_VALID = 1u << 0;
static constexpr uint32_t RTC_ARM_TAIL_VALID = 1u << 1;
static constexpr uint32_t RTC_WDT_MAGIC = 0x57444343u;
static constexpr uint16_t RTC_WDT_SCHEMA = 1;
static constexpr uint32_t RTC_CRASH_TIME_MAGIC = 0x54434341u;
static constexpr uint16_t RTC_CRASH_TIME_SCHEMA = 1;
static constexpr time_t VALID_CRASH_TIME_MIN_EPOCH = 1609459200;
static constexpr uint32_t CRASH_TIME_CAPTURE_INTERVAL_MS = 1000;

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

struct RtcCrashArmRecord {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t checksum;
    uint32_t flags;
    uint32_t dump_size;
    uint32_t dump_tail;
    char firmware[AC_CRASH_FIRMWARE_VERSION_MAX];
};

struct RtcTaskWatchdogRecord {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t checksum;
    char detail[AC_CRASH_REASON_MAX];
};

struct RtcCrashTimeRecord {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t checksum;
    uint32_t epoch_s;
};

// The linker lays these symbols out in reverse declaration order. Keep new
// extension records before the three original records so their retained
// addresses stay stable across firmware updates.
RTC_NOINIT_ATTR RtcCrashTimeRecord rtc_crash_time_record;
RTC_NOINIT_ATTR RtcPanicRecord rtc_panic_record;
RTC_NOINIT_ATTR RtcCrashArmRecord rtc_crash_arm_record;
RTC_NOINIT_ATTR RtcTaskWatchdogRecord rtc_task_watchdog_record;
DRAM_ATTR volatile bool rtc_task_watchdog_pending;
DRAM_ATTR volatile uint32_t crash_utc_epoch_s;

struct FlashDumpFingerprint {
    bool valid = false;
    bool tail_valid = false;
    uint32_t size = 0;
    uint32_t tail = 0;
};

template <typename Record>
uint32_t IRAM_ATTR rtc_record_checksum(Record &record) {
    const uint32_t saved = record.checksum;
    record.checksum = 0;

    const size_t offset = offsetof(Record, schema);
    const uint32_t result = esp_rom_crc32_le(
        0,
        reinterpret_cast<const uint8_t *>(&record) + offset,
        sizeof(record) - offset);
    record.checksum = saved;
    return result;
}

template <typename Record>
bool rtc_record_valid(Record record, uint32_t magic, uint16_t schema) {
    if (record.magic != magic || record.schema != schema ||
        record.size != sizeof(Record)) {
        return false;
    }

    return record.checksum == rtc_record_checksum(record);
}

template <typename Record>
void IRAM_ATTR finish_rtc_record(Record &record, uint32_t magic) {
    record.checksum = rtc_record_checksum(record);
    __asm__ __volatile__("memw" ::: "memory");
    record.magic = magic;
    __asm__ __volatile__("memw" ::: "memory");
}

void clear_rtc_panic_record() {
    rtc_panic_record.magic = 0;
    memset(&rtc_panic_record, 0, sizeof(rtc_panic_record));
    rtc_panic_record.schema = RTC_PANIC_SCHEMA;
    rtc_panic_record.size = sizeof(rtc_panic_record);
    rtc_panic_record.core = -1;
    finish_rtc_record(rtc_panic_record, RTC_PANIC_MAGIC);

    memset(&rtc_task_watchdog_record, 0,
           sizeof(rtc_task_watchdog_record));
    memset(&rtc_crash_time_record, 0, sizeof(rtc_crash_time_record));
    rtc_task_watchdog_pending = false;
}

void IRAM_ATTR begin_rtc_crash_time() {
    rtc_crash_time_record.magic = 0;
    __asm__ __volatile__("memw" ::: "memory");
}

void IRAM_ATTR finish_rtc_crash_time() {
    rtc_crash_time_record.schema = RTC_CRASH_TIME_SCHEMA;
    rtc_crash_time_record.size = sizeof(rtc_crash_time_record);
    rtc_crash_time_record.epoch_s = crash_utc_epoch_s;
    finish_rtc_record(rtc_crash_time_record, RTC_CRASH_TIME_MAGIC);
}

void IRAM_ATTR copy_rtc_text(char *out, size_t out_size, const char *text) {
    if (!out || out_size == 0) return;

    size_t i = 0;
    if (text) {
        while (i + 1 < out_size && text[i]) {
            out[i] = text[i];
            ++i;
        }
    }
    out[i] = 0;
    while (++i < out_size) out[i] = 0;
}

void IRAM_ATTR append_rtc_text(void *opaque, const char *text) {
    auto *record = static_cast<RtcTaskWatchdogRecord *>(opaque);
    if (!record || !text) return;

    size_t used = 0;
    while (used < sizeof(record->detail) && record->detail[used]) ++used;
    size_t source = 0;
    while (used + 1 < sizeof(record->detail) && text[source]) {
        record->detail[used++] = text[source++];
    }
    record->detail[used] = 0;
}

const char *IRAM_ATTR armed_firmware() {
    if (rtc_crash_arm_record.magic != RTC_ARM_MAGIC ||
        rtc_crash_arm_record.schema != RTC_ARM_SCHEMA ||
        rtc_crash_arm_record.size != sizeof(rtc_crash_arm_record)) {
        return nullptr;
    }
    return rtc_crash_arm_record.firmware;
}

void IRAM_ATTR capture_rtc_task_watchdog() {
    rtc_task_watchdog_pending = true;
    begin_rtc_crash_time();

    rtc_panic_record.magic = 0;
    rtc_panic_record.schema = RTC_PANIC_SCHEMA;
    rtc_panic_record.size = sizeof(rtc_panic_record);
    rtc_panic_record.flags = RTC_PANIC_CAPTURED | RTC_PANIC_TASK_WDT;
    rtc_panic_record.core = xPortGetCoreID();
    rtc_panic_record.pc = 0;
    rtc_panic_record.backtrace_depth = 0;
    for (size_t i = 0; i < AC_CRASH_BACKTRACE_MAX; ++i) {
        rtc_panic_record.backtrace[i] = 0;
    }
    copy_rtc_text(rtc_panic_record.firmware,
                  sizeof(rtc_panic_record.firmware), armed_firmware());
    finish_rtc_record(rtc_panic_record, RTC_PANIC_MAGIC);

    rtc_task_watchdog_record.magic = 0;
    rtc_task_watchdog_record.schema = RTC_WDT_SCHEMA;
    rtc_task_watchdog_record.size = sizeof(rtc_task_watchdog_record);
    rtc_task_watchdog_record.detail[0] = 0;
    (void)esp_task_wdt_print_triggered_tasks(
        append_rtc_text, &rtc_task_watchdog_record, nullptr);
    finish_rtc_record(rtc_task_watchdog_record, RTC_WDT_MAGIC);
    finish_rtc_crash_time();
}

void IRAM_ATTR capture_rtc_panic(arduino_panic_info_t *info, void *) {
    if (!info) return;

    begin_rtc_crash_time();

    rtc_panic_record.magic = 0;
    rtc_panic_record.schema = RTC_PANIC_SCHEMA;
    rtc_panic_record.size = sizeof(rtc_panic_record);
    rtc_panic_record.flags = RTC_PANIC_CAPTURED |
        (rtc_task_watchdog_pending ? RTC_PANIC_TASK_WDT : 0);
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

    copy_rtc_text(rtc_panic_record.firmware,
                  sizeof(rtc_panic_record.firmware), armed_firmware());
    if (!rtc_task_watchdog_pending) {
        rtc_task_watchdog_record.magic = 0;
    }
    finish_rtc_record(rtc_panic_record, RTC_PANIC_MAGIC);
    finish_rtc_crash_time();
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

bool format_crash_time(uint32_t epoch_s, char *out, size_t out_size) {
    if (!out || out_size < AC_CRASH_OCCURRED_AT_MAX ||
        epoch_s < static_cast<uint32_t>(VALID_CRASH_TIME_MIN_EPOCH)) {
        return false;
    }

    const time_t seconds = static_cast<time_t>(epoch_s);
    struct tm utc = {};
    if (!gmtime_r(&seconds, &utc)) return false;

    return strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
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

void import_rtc_snapshot(CrashDiagnosticsSnapshot &out) {
    const RtcPanicRecord retained = rtc_panic_record;
    if (!rtc_record_valid(retained, RTC_PANIC_MAGIC, RTC_PANIC_SCHEMA) ||
        !(retained.flags & RTC_PANIC_CAPTURED)) {
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
    out.rtc_task_watchdog = retained.flags & RTC_PANIC_TASK_WDT;
    if (retained.flags & RTC_DUMP_RELATION_KNOWN) {
        out.dump_relation = retained.flags & RTC_DUMP_RELATION_CURRENT
                                ? CrashDumpRelation::Current
                                : CrashDumpRelation::Stale;
    }

    const RtcTaskWatchdogRecord watchdog = rtc_task_watchdog_record;
    if (out.rtc_task_watchdog &&
        rtc_record_valid(watchdog, RTC_WDT_MAGIC, RTC_WDT_SCHEMA)) {
        copy_text(out.rtc_detail, sizeof(out.rtc_detail), watchdog.detail);
    }

    const RtcCrashTimeRecord crash_time = rtc_crash_time_record;
    if (rtc_record_valid(crash_time,
                         RTC_CRASH_TIME_MAGIC,
                         RTC_CRASH_TIME_SCHEMA)) {
        (void)format_crash_time(crash_time.epoch_s,
                                out.occurred_at,
                                sizeof(out.occurred_at));
    }
}

FlashDumpFingerprint read_dump_fingerprint(
    const esp_partition_t *partition) {
    FlashDumpFingerprint fingerprint;
    if (!partition ||
        esp_partition_read(partition, 0, &fingerprint.size,
                           sizeof(fingerprint.size)) != ESP_OK) {
        return fingerprint;
    }

    fingerprint.valid = true;
    if (fingerprint.size == UINT32_MAX ||
        fingerprint.size < sizeof(uint32_t) ||
        fingerprint.size > partition->size) {
        return fingerprint;
    }

    fingerprint.tail_valid = esp_partition_read(
        partition, fingerprint.size - sizeof(fingerprint.tail),
        &fingerprint.tail, sizeof(fingerprint.tail)) == ESP_OK;
    return fingerprint;
}

bool fingerprint_matches_arm(
    const FlashDumpFingerprint &fingerprint,
    const RtcCrashArmRecord &arm) {
    if (!fingerprint.valid ||
        !(arm.flags & RTC_ARM_FINGERPRINT_VALID) ||
        fingerprint.size != arm.dump_size) {
        return false;
    }
    const bool arm_tail_valid = arm.flags & RTC_ARM_TAIL_VALID;
    return fingerprint.tail_valid == arm_tail_valid &&
        (!fingerprint.tail_valid || fingerprint.tail == arm.dump_tail);
}

void record_dump_relation(
    CrashDiagnosticsSnapshot &snapshot,
    const RtcCrashArmRecord &previous_arm,
    const FlashDumpFingerprint &fingerprint) {
    if (!snapshot.rtc_panic_available ||
        snapshot.dump_relation != CrashDumpRelation::Unknown ||
        !rtc_record_valid(previous_arm, RTC_ARM_MAGIC, RTC_ARM_SCHEMA) ||
        !fingerprint.valid) {
        return;
    }

    snapshot.dump_relation = fingerprint_matches_arm(
        fingerprint, previous_arm)
        ? CrashDumpRelation::Stale
        : CrashDumpRelation::Current;
    rtc_panic_record.magic = 0;
    rtc_panic_record.flags |= RTC_DUMP_RELATION_KNOWN;
    if (snapshot.dump_relation == CrashDumpRelation::Current) {
        rtc_panic_record.flags |= RTC_DUMP_RELATION_CURRENT;
    } else {
        rtc_panic_record.flags &= ~RTC_DUMP_RELATION_CURRENT;
    }
    finish_rtc_record(rtc_panic_record, RTC_PANIC_MAGIC);
}

void arm_crash_capture(const FlashDumpFingerprint &fingerprint) {
    rtc_crash_arm_record.magic = 0;
    memset(&rtc_crash_arm_record, 0, sizeof(rtc_crash_arm_record));
    rtc_crash_arm_record.schema = RTC_ARM_SCHEMA;
    rtc_crash_arm_record.size = sizeof(rtc_crash_arm_record);
    if (fingerprint.valid) {
        rtc_crash_arm_record.flags |= RTC_ARM_FINGERPRINT_VALID;
        rtc_crash_arm_record.dump_size = fingerprint.size;
    }
    if (fingerprint.tail_valid) {
        rtc_crash_arm_record.flags |= RTC_ARM_TAIL_VALID;
        rtc_crash_arm_record.dump_tail = fingerprint.tail;
    }
    copy_text(rtc_crash_arm_record.firmware,
              sizeof(rtc_crash_arm_record.firmware), aircannect_version());
    finish_rtc_record(rtc_crash_arm_record, RTC_ARM_MAGIC);
}

}  // namespace

extern "C" void IRAM_ATTR esp_task_wdt_isr_user_handler(void) {
    capture_rtc_task_watchdog();
}

const char *crash_dump_state_name(CrashDumpState state) {
    switch (state) {
        case CrashDumpState::Unsupported: return "unsupported";
        case CrashDumpState::Empty: return "empty";
        case CrashDumpState::Available: return "available";
        case CrashDumpState::Invalid: return "invalid";
    }
    return "unknown";
}

const char *crash_dump_relation_name(CrashDumpRelation relation) {
    switch (relation) {
        case CrashDumpRelation::Unknown: return "unknown";
        case CrashDumpRelation::Current: return "current";
        case CrashDumpRelation::Stale: return "stale";
    }
    return "unknown";
}

bool CrashDiagnostics::begin() {
    import_rtc_snapshot(snapshot_);
    if (!rtc_record_valid(
            rtc_panic_record, RTC_PANIC_MAGIC, RTC_PANIC_SCHEMA)) {
        clear_rtc_panic_record();
    }
    set_arduino_panic_handler(capture_rtc_panic, nullptr);

    if (!mutex_) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }
    if (!mutex_) return false;

    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const RtcCrashArmRecord previous_arm = rtc_crash_arm_record;
    refresh_dump_locked();
    const FlashDumpFingerprint fingerprint = read_dump_fingerprint(
        static_cast<const esp_partition_t *>(partition_));
    record_dump_relation(snapshot_, previous_arm, fingerprint);
    arm_crash_capture(fingerprint);
    xSemaphoreGive(mutex_);
    poll();
    return true;
}

void CrashDiagnostics::poll() {
    const uint32_t now_ms = millis();
    if (next_time_capture_ms_ &&
        static_cast<int32_t>(now_ms - next_time_capture_ms_) < 0) {
        return;
    }
    next_time_capture_ms_ = now_ms + CRASH_TIME_CAPTURE_INTERVAL_MS;

    const time_t now = time(nullptr);
    if (now < VALID_CRASH_TIME_MIN_EPOCH ||
        static_cast<uint64_t>(now) > UINT32_MAX) {
        return;
    }

    crash_utc_epoch_s = static_cast<uint32_t>(now);
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
    const bool rtc_task_watchdog = snapshot_.rtc_task_watchdog;
    const CrashDumpRelation dump_relation = snapshot_.dump_relation;
    char occurred_at[sizeof(snapshot_.occurred_at)] = {};
    char rtc_detail[sizeof(snapshot_.rtc_detail)] = {};
    memcpy(occurred_at, snapshot_.occurred_at, sizeof(occurred_at));
    memcpy(rtc_firmware, snapshot_.rtc_firmware, sizeof(rtc_firmware));
    memcpy(rtc_backtrace, snapshot_.rtc_backtrace, sizeof(rtc_backtrace));
    memcpy(rtc_detail, snapshot_.rtc_detail, sizeof(rtc_detail));

    snapshot_ = {};
    snapshot_.dump_relation = dump_relation;
    memcpy(snapshot_.occurred_at, occurred_at, sizeof(occurred_at));
    snapshot_.rtc_panic_available = rtc_available;
    memcpy(snapshot_.rtc_firmware, rtc_firmware, sizeof(rtc_firmware));
    snapshot_.rtc_core = rtc_core;
    snapshot_.rtc_pc = rtc_pc;
    snapshot_.rtc_backtrace_depth = rtc_depth;
    memcpy(snapshot_.rtc_backtrace, rtc_backtrace, sizeof(rtc_backtrace));
    snapshot_.rtc_backtrace_corrupt = rtc_corrupt;
    snapshot_.rtc_backtrace_continues = rtc_continues;
    snapshot_.rtc_task_watchdog = rtc_task_watchdog;
    memcpy(snapshot_.rtc_detail, rtc_detail, sizeof(rtc_detail));

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
    snapshot_.dump_stored_size = stored_size;

    const esp_err_t check = esp_core_dump_image_check();
    if (check == ESP_ERR_NOT_FOUND) {
        snapshot_.dump_state = CrashDumpState::Empty;
        return;
    }
    if (check != ESP_OK) {
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
            clear_rtc_panic_record();
            snapshot_ = {};
            snapshot_.dump_state = CrashDumpState::Unsupported;
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

    clear_rtc_panic_record();
    snapshot_ = {};
    refresh_dump_locked();
    arm_crash_capture(read_dump_fingerprint(
        static_cast<const esp_partition_t *>(partition_)));
    xSemaphoreGive(mutex_);
    return true;
}

void CrashDiagnostics::log_previous_crash() const {
    CrashDiagnosticsSnapshot snapshot;
    if (!copy_snapshot(snapshot)) return;

    if (snapshot.dump_state == CrashDumpState::Available) {
        const bool task_watchdog = snapshot.rtc_task_watchdog ||
            strstr(snapshot.reason, "Task watchdog") != nullptr;
        const char *reason = task_watchdog
                                 ? "task_watchdog"
                                 : (snapshot.reason[0]
                                        ? snapshot.reason
                                        : "panic");

        Log::logf(
            CAT_GENERAL, LOG_WARN,
            "[CRASH] panic occurred=%s task=%s reason=%.48s pc=0x%08lx\n",
            snapshot.occurred_at[0] ? snapshot.occurred_at : "--",
            snapshot.task[0] ? snapshot.task : "--",
            reason,
            static_cast<unsigned long>(snapshot.exception_pc));
        if (!snapshot.rtc_panic_available ||
            snapshot.dump_relation == CrashDumpRelation::Current) {
            return;
        }
    }

    if (snapshot.dump_state == CrashDumpState::Invalid) {
        if (!snapshot.rtc_panic_available &&
            !crash_reset_reason(esp_reset_reason())) {
            return;
        }
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[CRASH] invalid dump occurred=%s error=%s bytes=%u\n",
                  snapshot.occurred_at[0] ? snapshot.occurred_at : "--",
                  snapshot.dump_error[0] ? snapshot.dump_error : "unknown",
                  static_cast<unsigned>(snapshot.dump_stored_size));
    }

    if (snapshot.rtc_panic_available) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[CRASH] breadcrumb occurred=%s source=%s core=%d "
                  "pc=0x%08lx depth=%u\n",
                  snapshot.occurred_at[0] ? snapshot.occurred_at : "--",
                  snapshot.rtc_task_watchdog ? "task_watchdog" : "panic",
                  static_cast<int>(snapshot.rtc_core),
                  static_cast<unsigned long>(snapshot.rtc_pc),
                  static_cast<unsigned>(snapshot.rtc_backtrace_depth));
    }
}

}  // namespace aircannect
