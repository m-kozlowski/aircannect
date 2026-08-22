#include "report_parser.h"

#include <stdio.h>

#include "spool_event_parser.h"

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

}  // namespace

bool report_validate_spool_for_source(const ReportSpoolResult &result,
                                      ReportSourceId source,
                                      char *error,
                                      size_t error_len) {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        set_error(error, error_len, "unknown_report_source");
        return false;
    }
    return spool_result_valid_for_type(result, def->spool_type,
                                       error, error_len);
}

bool report_parse_summary_spool(const ReportSpoolResult &result,
                                ReportSummaryRecordCallback callback,
                                void *context,
                                char *error,
                                size_t error_len) {
    if (!report_validate_spool_for_source(result,
                                          ReportSourceId::Summary,
                                          error,
                                          error_len)) {
        return false;
    }
    return report_parse_summary_records(result.payload.data(),
                                        result.payload.size(),
                                        callback,
                                        context,
                                        error,
                                        error_len);
}

}  // namespace aircannect
