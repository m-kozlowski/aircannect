#pragma once

#include <stdint.h>

namespace aircannect {

class EdfReportCatalogRetry {
public:
    bool schedule_for_error(const char *error, uint32_t now_ms);
    void mark_started();
    void reset();

    bool pending() const { return due_ms_ != 0; }
    bool due(uint32_t now_ms) const;
    uint32_t remaining_ms(uint32_t now_ms) const;
    uint8_t attempts() const { return attempts_; }

private:
    uint32_t due_ms_ = 0;
    uint8_t attempts_ = 0;
};

}  // namespace aircannect
