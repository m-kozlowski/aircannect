#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_parser.h"

namespace aircannect {

class ReportCacheCoverageMarks {
public:
    ~ReportCacheCoverageMarks();

    bool begin();
    bool reset();
    void note_chunk(const ReportSummaryRecord *records,
                    size_t record_count,
                    const ReportParsedChunk &chunk);

    int64_t extent(size_t record_index) const;

private:
    int64_t *extent_ms_ = nullptr;
};

}  // namespace aircannect
