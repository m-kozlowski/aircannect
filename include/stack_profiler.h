#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board.h"

#if AC_STACK_PROFILE_ENABLED
namespace aircannect {

enum class StackProfileTask : uint8_t {
    Loop,
    AsyncTcp,
    ExportTask,
    EdfStorage,
    OximetrySensor,
    Count,
};

struct StackProfileSample {
    StackProfileTask task = StackProfileTask::Loop;
    bool valid = false;
    uint32_t free_bytes = 0;
};

class StackProfiler {
public:
    void poll(uint32_t now_ms,
              const StackProfileSample *samples,
              size_t count);

private:
    struct Slot {
        bool seen = false;
        uint32_t min_free_bytes = UINT32_MAX;
    };

    Slot slots_[static_cast<size_t>(StackProfileTask::Count)];
    uint32_t next_sample_ms_ = 0;
    uint32_t next_summary_ms_ = 0;

    void log_sample(StackProfileTask task,
                    uint32_t free_bytes,
                    const char *reason) const;
    void log_summary() const;
};

}  // namespace aircannect
#endif
