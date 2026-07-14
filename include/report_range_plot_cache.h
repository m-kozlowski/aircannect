#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_spool_types.h"

namespace aircannect {

enum class ReportRangePlotRead : uint8_t {
    Building,
    Empty,
    Error,
    Ready,
};

struct ReportRangePlotRequest {
    bool active = false;
    size_t index = 0;
    uint64_t night_start_ms = 0;
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    int64_t from_ms = 0;
    int64_t to_ms = 0;
};

// Owns the completed zoom-plot LRU and the single latest-wins build request.
// ReportResultSlotCache provides synchronization and PSRAM placement.
class ReportRangePlotCache {
public:
    ReportRangePlotRead read_or_request(
        size_t index,
        uint64_t night_start_ms,
        const char *etag,
        int64_t from_ms,
        int64_t to_ms,
        std::shared_ptr<ReportSpoolBuffer> &out);
    bool request_snapshot(ReportRangePlotRequest &out) const;

    bool finish_request(size_t index,
                        uint64_t night_start_ms,
                        const char *etag,
                        int64_t from_ms,
                        int64_t to_ms,
                        const std::shared_ptr<ReportSpoolBuffer> &plot,
                        bool empty);
    void fail_request(size_t index,
                      uint64_t night_start_ms,
                      const char *etag,
                      int64_t from_ms,
                      int64_t to_ms);

    void reset(bool clear_ready);
    void invalidate(uint64_t night_start_ms, bool all);

private:
    struct Entry {
        bool valid = false;
        bool empty = false;
        uint64_t night_start_ms = 0;
        char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
        int64_t from_ms = 0;
        int64_t to_ms = 0;
        uint32_t last_used = 0;
        std::shared_ptr<ReportSpoolBuffer> bytes;
    };

    bool request_matches(size_t index,
                         uint64_t night_start_ms,
                         const char *etag,
                         int64_t from_ms,
                         int64_t to_ms) const;
    void begin_request(size_t index,
                       uint64_t night_start_ms,
                       const char *etag,
                       int64_t from_ms,
                       int64_t to_ms);
    void clear_request();

    void clear_entry(Entry &entry);
    void trim_for(size_t incoming_bytes, size_t protected_index);

    Entry entries_[AC_REPORT_RANGE_CACHE_SLOT_MAX] = {};
    uint32_t tick_ = 0;
    ReportRangePlotRequest request_;
};

}  // namespace aircannect
