#include "report_event_dedupe.h"

namespace aircannect {
namespace {

bool same_report_event(const ReportEventRecord &a,
                       const ReportEventRecord &b) {
    return a.start_ms == b.start_ms &&
           a.duration_ms == b.duration_ms &&
           a.code == b.code &&
           a.flags == b.flags;
}

}  // namespace

bool report_event_seen(const ReportSpoolBuffer &seen,
                       const ReportEventRecord &event) {
    const size_t count = seen.size() / report_event_record_wire_size();
    for (size_t i = 0; i < count; ++i) {
        ReportEventRecord current;
        if (!report_read_event_record(seen.data(),
                                      seen.size(),
                                      i,
                                      current)) {
            continue;
        }

        if (same_report_event(current, event)) return true;
    }

    return false;
}

bool remember_report_event(ReportSpoolBuffer &seen,
                           const ReportEventRecord &event) {
    if (report_event_seen(seen, event)) return true;
    return report_append_event_record(seen, event);
}

}  // namespace aircannect
