#include "report_result_identity.h"

#include <stdio.h>
#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"

namespace aircannect {

ReportResultIdentity::~ReportResultIdentity() {
    Memory::free(indexed_night_);
}

void ReportResultIdentity::clear() {
    if (indexed_night_) *indexed_night_ = {};
    etag_[0] = '\0';
}

bool ReportResultIdentity::set(const ReportIndexedNight &night,
                               const char *etag) {
    if (!indexed_night_) {
        indexed_night_ = static_cast<ReportIndexedNight *>(
            Memory::alloc_large(sizeof(ReportIndexedNight), false));
        if (!indexed_night_) {
            log_report_alloc_failed("result_identity",
                                    sizeof(ReportIndexedNight));
            return false;
        }
    }

    *indexed_night_ = night;
    snprintf(etag_, sizeof(etag_), "%s", etag ? etag : "");
    return true;
}

bool ReportResultIdentity::valid() const {
    return indexed_night_ && etag_[0] &&
           indexed_night_->summary.start_ms != 0;
}

bool ReportResultIdentity::etag_matches(const char *etag) const {
    return etag && etag[0] && strcmp(etag_, etag) == 0;
}

void report_format_result_etag(const ReportSummaryRecord &record,
                               uint64_t source_signature,
                               uint32_t night_epoch,
                               char *out,
                               size_t out_size) {
    constexpr uint32_t REPORT_RESULT_ETAG_VERSION = 32;

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
