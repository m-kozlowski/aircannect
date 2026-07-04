#pragma once

#include <stddef.h>

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"

namespace aircannect {

class ScopedIndexedNight {
public:
    explicit ScopedIndexedNight(const char *context)
        : context_(context),
          night_(static_cast<ReportIndexedNight *>(
              Memory::alloc_large(sizeof(ReportIndexedNight), false))) {
        if (!night_) {
            log_report_alloc_failed(context_, sizeof(ReportIndexedNight));
        }
    }

    ~ScopedIndexedNight() {
        Memory::free(night_);
    }

    ScopedIndexedNight(const ScopedIndexedNight &) = delete;
    ScopedIndexedNight &operator=(const ScopedIndexedNight &) = delete;

    explicit operator bool() const { return night_ != nullptr; }
    ReportIndexedNight &get() { return *night_; }
    const ReportIndexedNight &get() const { return *night_; }
    ReportIndexedNight *operator->() { return night_; }
    const ReportIndexedNight *operator->() const { return night_; }

private:
    const char *context_ = nullptr;
    ReportIndexedNight *night_ = nullptr;
};

}  // namespace aircannect
