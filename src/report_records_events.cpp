#include "report_records.h"

#include "report_records_internal.h"

#include <limits.h>

namespace aircannect {

using report_records_detail::EVENT_RECORD_WIRE_SIZE;
using report_records_detail::get_le16;
using report_records_detail::get_le32;
using report_records_detail::get_le64;
using report_records_detail::put_le16;
using report_records_detail::put_le32;
using report_records_detail::put_le64;
using report_records_detail::valid_timestamp;

size_t report_event_record_wire_size() {
    return EVENT_RECORD_WIRE_SIZE;
}

bool report_event_overlaps_window(const ReportEventRecord &event,
                                  int64_t window_start_ms,
                                  int64_t window_end_ms,
                                  int64_t edge_tolerance_ms) {
    if (!valid_timestamp(event.start_ms) ||
        window_end_ms <= window_start_ms ||
        event.duration_ms < 0) {
        return false;
    }

    const int64_t tolerance =
        edge_tolerance_ms > 0 ? edge_tolerance_ms : 0;
    const int64_t expanded_start = window_start_ms - tolerance;
    const int64_t expanded_end = window_end_ms + tolerance;

    if (event.duration_ms == 0) {
        return event.start_ms >= expanded_start &&
               event.start_ms < expanded_end;
    }

    const int64_t duration_ms = static_cast<int64_t>(event.duration_ms);
    if (duration_ms > INT64_MAX - event.start_ms) return false;

    const int64_t event_end_ms = event.start_ms + duration_ms;
    if (event_end_ms <= event.start_ms) return false;

    return event.start_ms < expanded_end && event_end_ms > expanded_start;
}

bool report_append_event_record(ReportSpoolBuffer &out,
                                const ReportEventRecord &event) {
    if (!valid_timestamp(event.start_ms) || event.duration_ms < 0) {
        return false;
    }

    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(EVENT_RECORD_WIRE_SIZE, offset);
    if (!dst) return false;

    put_le64(dst + 0, static_cast<uint64_t>(event.start_ms));
    put_le32(dst + 8, static_cast<uint32_t>(event.duration_ms));
    put_le16(dst + 12, event.code);
    put_le16(dst + 14, event.flags);
    return true;
}

bool report_read_event_record(const uint8_t *data,
                              size_t len,
                              size_t index,
                              ReportEventRecord &event) {
    if (!data || index > SIZE_MAX / EVENT_RECORD_WIRE_SIZE) return false;

    const size_t offset = index * EVENT_RECORD_WIRE_SIZE;
    if (offset > len || len - offset < EVENT_RECORD_WIRE_SIZE) return false;

    event.start_ms = static_cast<int64_t>(get_le64(data + offset));
    event.duration_ms = static_cast<int32_t>(get_le32(data + offset + 8));
    event.code = get_le16(data + offset + 12);
    event.flags = get_le16(data + offset + 14);
    return valid_timestamp(event.start_ms) && event.duration_ms >= 0;
}

}  // namespace aircannect
