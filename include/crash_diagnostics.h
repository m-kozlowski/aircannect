#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class LargeByteBuffer;

static constexpr size_t AC_CRASH_BACKTRACE_MAX = 16;
static constexpr size_t AC_CRASH_REASON_MAX = 160;
static constexpr size_t AC_CRASH_TASK_NAME_MAX = 17;
static constexpr size_t AC_CRASH_ELF_SHA_MAX = 10;
static constexpr size_t AC_CRASH_FIRMWARE_VERSION_MAX = 48;
static constexpr size_t AC_CRASH_ERROR_MAX = 40;
static constexpr size_t AC_CRASH_OCCURRED_AT_MAX = 21;

enum class CrashDumpState : uint8_t {
    Unsupported,
    Empty,
    Available,
    Invalid,
};

enum class CrashDumpRelation : uint8_t {
    Unknown,
    Current,
    Stale,
};

const char *crash_dump_state_name(CrashDumpState state);
const char *crash_dump_relation_name(CrashDumpRelation relation);

struct CrashDiagnosticsSnapshot {
    CrashDumpState dump_state = CrashDumpState::Unsupported;
    CrashDumpRelation dump_relation = CrashDumpRelation::Unknown;
    size_t dump_size = 0;
    size_t dump_stored_size = 0;
    char dump_error[AC_CRASH_ERROR_MAX] = {};

    bool summary_available = false;
    char occurred_at[AC_CRASH_OCCURRED_AT_MAX] = {};
    char task[AC_CRASH_TASK_NAME_MAX] = {};
    char reason[AC_CRASH_REASON_MAX] = {};
    char elf_sha[AC_CRASH_ELF_SHA_MAX] = {};
    uint32_t exception_pc = 0;
    uint32_t exception_cause = 0;
    uint32_t exception_address = 0;
    uint32_t backtrace[AC_CRASH_BACKTRACE_MAX] = {};
    uint8_t backtrace_depth = 0;
    bool backtrace_corrupt = false;

    bool rtc_panic_available = false;
    char rtc_firmware[AC_CRASH_FIRMWARE_VERSION_MAX] = {};
    int8_t rtc_core = -1;
    uint32_t rtc_pc = 0;
    uint32_t rtc_backtrace[AC_CRASH_BACKTRACE_MAX] = {};
    uint8_t rtc_backtrace_depth = 0;
    bool rtc_backtrace_corrupt = false;
    bool rtc_backtrace_continues = false;
    bool rtc_task_watchdog = false;
    char rtc_detail[AC_CRASH_REASON_MAX] = {};
};

class CrashDiagnostics {
public:
    bool begin();
    void poll();
    void log_previous_crash() const;

    bool copy_snapshot(CrashDiagnosticsSnapshot &out) const;
    std::shared_ptr<const LargeByteBuffer> copy_dump(
        char *error,
        size_t error_size) const;
    bool clear(char *error, size_t error_size);

private:
    void refresh_dump_locked();

    StaticSemaphore_t mutex_storage_ = {};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    const void *partition_ = nullptr;
    uint32_t next_time_capture_ms_ = 0;
    CrashDiagnosticsSnapshot snapshot_;
};

}  // namespace aircannect
