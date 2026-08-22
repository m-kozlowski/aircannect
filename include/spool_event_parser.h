#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_spool_types.h"

namespace aircannect {

struct SpoolEventRecord {
    uint16_t type = 0;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int32_t duration_ms = 0;
};

struct SpoolEventParseStats {
    size_t records = 0;
    size_t malformed_records = 0;
};

using SpoolEventRecordCallback =
    bool (*)(void *context, const SpoolEventRecord &record);

bool spool_result_valid_for_type(const ReportSpoolResult &result,
                                 const char *expected_type,
                                 char *error,
                                 size_t error_len);
bool spool_parse_event_records(const uint8_t *data,
                               size_t len,
                               SpoolEventRecordCallback callback,
                               void *context,
                               SpoolEventParseStats *stats = nullptr);

}  // namespace aircannect
