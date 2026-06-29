#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_proto.h"

namespace aircannect {

static constexpr size_t AC_REPORT_SUMMARY_RECORD_CODEC_SIZE = 512;

bool report_summary_record_encode(uint8_t *raw,
                                  size_t raw_size,
                                  const ReportSummaryRecord &record);
bool report_summary_record_decode(const uint8_t *raw,
                                  size_t raw_size,
                                  ReportSummaryRecord &record);

}  // namespace aircannect
