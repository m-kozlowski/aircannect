#include "report_parser.h"

#include <stdio.h>
#include <string.h>

#include <string>

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

bool strings_match(const std::string &left, const char *right) {
    return right && strcmp(left.c_str(), right) == 0;
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
    if (!strings_match(result.spool_type, def->spool_type)) {
        set_error(error, error_len, "wrong_report_source");
        return false;
    }
    if (!result.complete) {
        set_error(error, error_len, "spool_incomplete");
        return false;
    }
    if (result.truncated) {
        set_error(error, error_len, "spool_truncated");
        return false;
    }
    if (!result.sha_ok) {
        set_error(error, error_len, "spool_hash_failed");
        return false;
    }
    if (!result.payload.data() || result.payload.size() == 0) {
        set_error(error, error_len, "spool_empty");
        return false;
    }
    set_error(error, error_len, "");
    return true;
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
