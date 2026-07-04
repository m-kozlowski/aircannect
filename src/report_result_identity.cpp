#include "report_result_identity.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {

void ReportResultIdentity::clear() {
    indexed_night_ = {};
    summary_ = {};
    etag_[0] = '\0';
}

void ReportResultIdentity::set(const ReportIndexedNight &night,
                               const char *etag) {
    indexed_night_ = night;
    summary_ = night.summary;
    snprintf(etag_, sizeof(etag_), "%s", etag ? etag : "");
}

bool ReportResultIdentity::valid() const {
    return etag_[0] && indexed_night_.summary.start_ms != 0;
}

bool ReportResultIdentity::etag_matches(const char *etag) const {
    return etag && etag[0] && strcmp(etag_, etag) == 0;
}

void report_format_result_etag(const ReportSummaryRecord &record,
                               uint64_t source_signature,
                               uint32_t night_epoch,
                               char *out,
                               size_t out_size) {
    constexpr uint32_t REPORT_RESULT_ETAG_VERSION = 29;

    if (!out || !out_size) return;

    snprintf(out,
             out_size,
             "%llu-%lu-%lu-%08lx%08lx-%lu-r%lu",
             static_cast<unsigned long long>(record.start_ms),
             static_cast<unsigned long>(record.duration_min),
             static_cast<unsigned long>(record.session_interval_count),
             static_cast<unsigned long>(source_signature >> 32),
             static_cast<unsigned long>(source_signature & 0xffffffffULL),
             static_cast<unsigned long>(night_epoch),
             static_cast<unsigned long>(REPORT_RESULT_ETAG_VERSION));
}

}  // namespace aircannect
