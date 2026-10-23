#include "irq_block_probe.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_debug_helpers.h>
#include <esp_private/cache_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <xtensa/core-macros.h>

#include "debug_log.h"

extern "C" {
extern volatile UBaseType_t port_uxCriticalNesting[portNUM_PROCESSORS];
BaseType_t __real_xPortEnterCriticalTimeout(portMUX_TYPE *, BaseType_t);
void __real_vPortExitCritical(portMUX_TYPE *);
void __real_spi_flash_disable_interrupts_caches_and_other_cpu();
void __real_spi_flash_enable_interrupts_caches_and_other_cpu();
}

namespace {

constexpr unsigned CAPACITY = 16;
constexpr uint32_t THRESHOLD_US = 200;

struct Record {
    uint32_t held_cycles;
    uintptr_t enter_pc;
    uintptr_t exit_pc;
    uintptr_t task;
    int64_t at_us;
    uint32_t kind;
    uint32_t callers[8];
};

struct Core {
    uint32_t start;
    uintptr_t enter_pc;
    uintptr_t task;
    bool active;
    uint32_t flash_start;
    uintptr_t flash_pc;
    bool flash_active;
    Record records[CAPACITY];
    unsigned write;
    unsigned read;
    unsigned dropped;
};

DRAM_ATTR Core cores[portNUM_PROCESSORS] = {};
DRAM_ATTR bool enabled = false;
DRAM_ATTR uint32_t cycles_per_us = 240;
DRAM_ATTR uint32_t threshold_cycles = THRESHOLD_US * 240;

void IRAM_ATTR record(Core &core, uint32_t kind, uint32_t held,
                      uintptr_t enter_pc, uintptr_t exit_pc,
                      uintptr_t task) {
    const unsigned write = __atomic_load_n(&core.write, __ATOMIC_RELAXED);
    if (write - __atomic_load_n(&core.read, __ATOMIC_ACQUIRE) >= CAPACITY) {
        __atomic_fetch_add(&core.dropped, 1U, __ATOMIC_RELAXED);
        return;
    }

    Record &out = core.records[write % CAPACITY];
    out.held_cycles = held;
    out.enter_pc = enter_pc;
    out.exit_pc = exit_pc;
    out.task = task;
    out.at_us = esp_timer_get_time();
    out.kind = kind;

    for (unsigned i = 0; i < 8; ++i) out.callers[i] = 0;

    if (!xPortInIsrContext() && spi_flash_cache_enabled()) {
        esp_backtrace_frame_t frame = {};
        esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
        for (unsigned i = 0; i < 8 && frame.next_pc; ++i) {
            if (!esp_backtrace_get_next_frame(&frame)) break;
            out.callers[i] = (frame.pc & 0x3fffffff) | 0x40000000;
        }
    }

    __atomic_store_n(&core.write, write + 1, __ATOMIC_RELEASE);
}

}  // namespace

extern "C" BaseType_t IRAM_ATTR __wrap_xPortEnterCriticalTimeout(
    portMUX_TYPE *mux, BaseType_t timeout) {
    const BaseType_t result = __real_xPortEnterCriticalTimeout(mux, timeout);
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || result != pdPASS) {
        return result;
    }

    const unsigned cpu = xPortGetCoreID();
    if (port_uxCriticalNesting[cpu] == 1) {
        Core &core = cores[cpu];
        core.start = XTHAL_GET_CCOUNT();
        core.enter_pc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        core.task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
        core.active = true;
    }

    return result;
}

extern "C" void IRAM_ATTR __wrap_vPortExitCritical(portMUX_TYPE *mux) {
    const unsigned cpu = xPortGetCoreID();
    Core &core = cores[cpu];
    if (__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) &&
        core.active && port_uxCriticalNesting[cpu] == 1) {
        const uint32_t held = XTHAL_GET_CCOUNT() - core.start;
        core.active = false;

        if (held >= threshold_cycles) {
            record(core, 0, held, core.enter_pc,
                   reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                   core.task);
        }
    }

    __real_vPortExitCritical(mux);
}

extern "C" void IRAM_ATTR
__wrap_spi_flash_disable_interrupts_caches_and_other_cpu() {
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) return;

    Core &core = cores[xPortGetCoreID()];
    core.flash_active = true;
    core.flash_start = XTHAL_GET_CCOUNT();
    core.flash_pc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
}

extern "C" void IRAM_ATTR
__wrap_spi_flash_enable_interrupts_caches_and_other_cpu() {
    Core &core = cores[xPortGetCoreID()];
    if (__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) && core.flash_active) {
        const uint32_t held = XTHAL_GET_CCOUNT() - core.flash_start;
        core.flash_active = false;

        if (held >= threshold_cycles) {
            record(core, 1, held, core.flash_pc,
                   reinterpret_cast<uintptr_t>(__builtin_return_address(0)), 0);
        }
    }

    __real_spi_flash_enable_interrupts_caches_and_other_cpu();
}

void irq_block_probe_begin() {
    cycles_per_us = getCpuFrequencyMhz();
    threshold_cycles = THRESHOLD_US * cycles_per_us;
    __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);

    Log::logf(CAT_GENERAL, LOG_INFO,
              "[IRQ] critical/flash probe enabled threshold_us=%lu cpu_mhz=%lu",
              static_cast<unsigned long>(THRESHOLD_US),
              static_cast<unsigned long>(cycles_per_us));
}

void irq_block_probe_poll() {
    // Printing never runs in the measured critical section.
    for (unsigned cpu = 0; cpu < portNUM_PROCESSORS; ++cpu) {
        Core &core = cores[cpu];
        const unsigned dropped =
            __atomic_exchange_n(&core.dropped, 0U, __ATOMIC_RELAXED);
        if (dropped) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[IRQ] core=%u records_dropped=%u", cpu, dropped);
        }

        const unsigned read = __atomic_load_n(&core.read, __ATOMIC_RELAXED);
        if (read == __atomic_load_n(&core.write, __ATOMIC_ACQUIRE)) continue;

        const Record item = core.records[read % CAPACITY];
        __atomic_store_n(&core.read, read + 1, __ATOMIC_RELEASE);
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[IRQ] kind=%s core=%u held_us=%lu enter=0x%08lx exit=0x%08lx task=0x%08lx at_ms=%lu",
                  item.kind ? "flash" : "critical", cpu,
                  static_cast<unsigned long>(item.held_cycles / cycles_per_us),
                  static_cast<unsigned long>((item.enter_pc & 0x3fffffff) | 0x40000000),
                  static_cast<unsigned long>((item.exit_pc & 0x3fffffff) | 0x40000000),
                  static_cast<unsigned long>(item.task),
                  static_cast<unsigned long>(item.at_us / 1000));
        if (item.callers[0]) {
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[IRQ] at_ms=%lu callers=%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                      static_cast<unsigned long>(item.at_us / 1000),
                      static_cast<unsigned long>(item.callers[0]),
                      static_cast<unsigned long>(item.callers[1]),
                      static_cast<unsigned long>(item.callers[2]),
                      static_cast<unsigned long>(item.callers[3]),
                      static_cast<unsigned long>(item.callers[4]),
                      static_cast<unsigned long>(item.callers[5]),
                      static_cast<unsigned long>(item.callers[6]),
                      static_cast<unsigned long>(item.callers[7]));
        }
    }
}
