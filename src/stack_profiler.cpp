#include "board.h"

#if AC_STACK_PROFILE_ENABLED
#include "stack_profiler.h"

#include <stdio.h>

#include "debug_log.h"
#include "edf_storage_worker.h"

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

constexpr StackProfileDef STACK_PROFILE_DEFS[] = {
    {"loop", CONFIG_ARDUINO_LOOP_STACK_SIZE},
    {"async_tcp", CONFIG_ASYNC_TCP_STACK_SIZE},
    {"bg_worker", AC_BG_WORKER_TASK_STACK},
    {"edf_storage", AC_EDF_STORAGE_TASK_STACK},
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

    if (static_cast<int32_t>(now_ms - next_summary_ms_) >= 0) {
        log_summary();
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

void StackProfiler::log_summary() const {
    char line[256];
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
