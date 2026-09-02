#include "board.h"

#if AC_STACK_PROFILE_ENABLED
#include "stack_profiler.h"

#include <stdio.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "storage_service.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/task.h>

namespace aircannect {
namespace {

struct StackProfileDef {
    const char *name = "";
    uint32_t stack_bytes = 0;
};

#ifndef CONFIG_ASYNC_TCP_STACK_SIZE
#define CONFIG_ASYNC_TCP_STACK_SIZE 8192
#endif

#ifndef CONFIG_ARDUINO_LOOP_STACK_SIZE
#define CONFIG_ARDUINO_LOOP_STACK_SIZE 8192
#endif

#ifndef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 5120
#endif

constexpr StackProfileDef STACK_PROFILE_DEFS[] = {
    {"loop", CONFIG_ARDUINO_LOOP_STACK_SIZE},
    {"async_tcp", CONFIG_ASYNC_TCP_STACK_SIZE},
    {"nimble_host", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE},
    {"as11_ble", AC_AS11_BLE_TASK_STACK},
    {"report", AC_REPORT_TASK_STACK},
    {"display", AC_DISPLAY_TASK_STACK},
    {"export", AC_EXPORT_TASK_STACK},
    {"storage", AC_STORAGE_SERVICE_TASK_STACK},
    {"oximetry", AC_OXIMETRY_SENSOR_TASK_STACK},
};

static_assert(
    sizeof(STACK_PROFILE_DEFS) / sizeof(STACK_PROFILE_DEFS[0]) ==
        static_cast<size_t>(StackProfileTask::Count),
    "stack profile table must match StackProfileTask");

const StackProfileDef &stack_profile_def(StackProfileTask task) {
    const size_t index = static_cast<size_t>(task);
    return STACK_PROFILE_DEFS[index];
}

uint32_t used_max_bytes(const StackProfileDef &def, uint32_t free_bytes) {
    return def.stack_bytes > free_bytes ? def.stack_bytes - free_bytes : 0;
}

struct TaskAllocation {
    size_t stack_bytes = 0;
    size_t tcb_bytes = 0;
};

struct TaskAllocationLookup {
    TaskStatus_t *tasks = nullptr;
    TaskAllocation *allocations = nullptr;
    size_t count = 0;
};

bool collect_task_allocations(walker_heap_into_t,
                              walker_block_info_t block,
                              void *context) {
    if (!block.used || !block.ptr || block.size == 0 || !context) return true;

    TaskAllocationLookup &lookup =
        *static_cast<TaskAllocationLookup *>(context);
    const uintptr_t block_begin = reinterpret_cast<uintptr_t>(block.ptr);
    const uintptr_t block_end = block_begin + block.size;

    for (size_t i = 0; i < lookup.count; ++i) {
        const uintptr_t stack =
            reinterpret_cast<uintptr_t>(lookup.tasks[i].pxStackBase);
        if (stack >= block_begin && stack < block_end) {
            lookup.allocations[i].stack_bytes = block.size;
        }

        const uintptr_t tcb =
            reinterpret_cast<uintptr_t>(lookup.tasks[i].xHandle);
        if (tcb >= block_begin && tcb < block_end) {
            lookup.allocations[i].tcb_bytes = block.size;
        }
    }
    return true;
}

const char *memory_region(const void *ptr) {
    if (esp_ptr_external_ram(ptr)) return "psram";
    if (esp_ptr_internal(ptr)) return "internal";
    return "static";
}

}  // namespace

void StackProfiler::poll(uint32_t now_ms,
                         const StackProfileSample *samples,
                         size_t count) {
    if (!samples || !count) return;
    if (next_sample_ms_ != 0 &&
        static_cast<int32_t>(now_ms - next_sample_ms_) < 0) {
        return;
    }
    next_sample_ms_ = now_ms + AC_STACK_PROFILE_SAMPLE_MS;
    if (next_summary_ms_ == 0) {
        next_summary_ms_ = now_ms + AC_STACK_PROFILE_SUMMARY_MS;
    }

    log_heap();

    for (size_t i = 0; i < count; ++i) {
        const StackProfileSample &sample = samples[i];
        const size_t index = static_cast<size_t>(sample.task);
        if (!sample.valid ||
            index >= static_cast<size_t>(StackProfileTask::Count)) {
            continue;
        }
        Slot &slot = slots_[index];
        if (!slot.seen || sample.free_bytes < slot.min_free_bytes) {
            const bool was_seen = slot.seen;
            slot.seen = true;
            slot.min_free_bytes = sample.free_bytes;
            log_sample(sample.task,
                       sample.free_bytes,
                       was_seen ? "new_min" : "initial");
        }
    }

    const size_t task_count = uxTaskGetNumberOfTasks();
    if (task_count != task_inventory_count_) {
        log_task_inventory();
        task_inventory_count_ = task_count;
    }

    if (static_cast<int32_t>(now_ms - next_summary_ms_) >= 0) {
        log_summary();
        log_task_inventory();
        next_summary_ms_ = now_ms + AC_STACK_PROFILE_SUMMARY_MS;
    }
}

void StackProfiler::log_sample(StackProfileTask task,
                               uint32_t free_bytes,
                               const char *reason) const {
    const StackProfileDef &def = stack_profile_def(task);
    Log::logf(CAT_GENERAL,
              LOG_INFO,
              "[STACK] %s task=%s stack=%u free_min=%u used_max=%u\n",
              reason ? reason : "sample",
              def.name,
              static_cast<unsigned>(def.stack_bytes),
              static_cast<unsigned>(free_bytes),
              static_cast<unsigned>(used_max_bytes(def, free_bytes)));
}

void StackProfiler::log_heap() const {
    const MemoryDetailStatus memory = Memory::detail_status();
    Log::logf(CAT_GENERAL,
              LOG_INFO,
              "[HEAP] runtime internal_free=%u internal_allocated=%u "
              "internal_largest=%u internal_min=%u internal_blocks=%u "
              "psram_free=%u psram_allocated=%u\n",
              static_cast<unsigned>(memory.internal_8bit.free_bytes),
              static_cast<unsigned>(memory.internal_8bit.allocated_bytes),
              static_cast<unsigned>(memory.internal_8bit.largest_free_block),
              static_cast<unsigned>(memory.internal_8bit.minimum_free_bytes),
              static_cast<unsigned>(memory.internal_8bit.allocated_blocks),
              static_cast<unsigned>(memory.psram_8bit.free_bytes),
              static_cast<unsigned>(memory.psram_8bit.allocated_bytes));
}

void StackProfiler::log_task_inventory() const {
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = static_cast<TaskStatus_t *>(Memory::calloc_large(
        capacity, sizeof(TaskStatus_t), false));
    TaskAllocation *allocations =
        static_cast<TaskAllocation *>(Memory::calloc_large(
            capacity, sizeof(TaskAllocation), false));
    if (!tasks || !allocations) {
        Memory::free(allocations);
        Memory::free(tasks);
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[STACK] task inventory allocation failed\n");
        return;
    }

    const UBaseType_t count = uxTaskGetSystemState(tasks, capacity, nullptr);
    TaskAllocationLookup lookup = {
        tasks,
        allocations,
        static_cast<size_t>(count),
    };
    heap_caps_walk_all(collect_task_allocations, &lookup);

    size_t internal_stack_bytes = 0;
    size_t psram_stack_bytes = 0;
    size_t internal_tcb_bytes = 0;
    for (UBaseType_t i = 0; i < count; ++i) {
        const char *stack_region = memory_region(tasks[i].pxStackBase);
        const char *tcb_region = memory_region(tasks[i].xHandle);
        if (strcmp(stack_region, "internal") == 0) {
            internal_stack_bytes += allocations[i].stack_bytes;
        } else if (strcmp(stack_region, "psram") == 0) {
            psram_stack_bytes += allocations[i].stack_bytes;
        }
        if (strcmp(tcb_region, "internal") == 0) {
            internal_tcb_bytes += allocations[i].tcb_bytes;
        }

        Log::logf(CAT_GENERAL,
                  LOG_INFO,
                  "[STACK] task=%s stack=%u free_min=%u memory=%s "
                  "tcb=%u tcb_memory=%s priority=%u\n",
                  tasks[i].pcTaskName ? tasks[i].pcTaskName : "?",
                  static_cast<unsigned>(allocations[i].stack_bytes),
                  static_cast<unsigned>(tasks[i].usStackHighWaterMark),
                  stack_region,
                  static_cast<unsigned>(allocations[i].tcb_bytes),
                  tcb_region,
                  static_cast<unsigned>(tasks[i].uxCurrentPriority));
    }

    Log::logf(CAT_GENERAL,
              LOG_INFO,
              "[STACK] inventory tasks=%u internal_stack=%u psram_stack=%u "
              "internal_tcb=%u\n",
              static_cast<unsigned>(count),
              static_cast<unsigned>(internal_stack_bytes),
              static_cast<unsigned>(psram_stack_bytes),
              static_cast<unsigned>(internal_tcb_bytes));
    Memory::free(allocations);
    Memory::free(tasks);
}

void StackProfiler::log_summary() const {
    char line[384];
    size_t used = snprintf(line, sizeof(line), "[STACK] summary");
    for (size_t i = 0;
         i < static_cast<size_t>(StackProfileTask::Count) && used < sizeof(line);
         ++i) {
        const Slot &slot = slots_[i];
        const StackProfileDef &def =
            stack_profile_def(static_cast<StackProfileTask>(i));
        int written = 0;
        if (slot.seen) {
            written = snprintf(line + used,
                               sizeof(line) - used,
                               " %s=%u/%u",
                               def.name,
                               static_cast<unsigned>(slot.min_free_bytes),
                               static_cast<unsigned>(def.stack_bytes));
        } else {
            written = snprintf(line + used,
                               sizeof(line) - used,
                               " %s=--/%u",
                               def.name,
                               static_cast<unsigned>(def.stack_bytes));
        }
        if (written < 0) break;
        used += static_cast<size_t>(written);
    }
    if (used >= sizeof(line)) {
        line[sizeof(line) - 1] = '\0';
    }
    Log::logf(CAT_GENERAL, LOG_INFO, "%s\n", line);
}

}  // namespace aircannect
#endif
