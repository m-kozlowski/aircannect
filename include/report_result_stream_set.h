#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_result_types.h"
#include "report_store.h"

namespace aircannect {

class ReportResultStreamSet {
public:
    void clear();

    ReportResultStream *data() { return streams_; }
    const ReportResultStream *data() const { return streams_; }

    size_t count() const { return count_; }
    bool full() const { return count_ >= AC_REPORT_RESULT_STREAM_MAX; }
    bool contains(size_t index) const {
        return index < count_ && index < AC_REPORT_RESULT_STREAM_MAX;
    }

    ReportResultStream &operator[](size_t index) { return streams_[index]; }
    const ReportResultStream &operator[](size_t index) const {
        return streams_[index];
    }

    bool add_or_update(ReportStoreChunkKind kind,
                       ReportSourceId source,
                       ReportSignalId signal,
                       const char *name,
                       bool required,
                       bool complete,
                       ReportResultStatus &status,
                       size_t &stream_index);

private:
    ReportResultStream streams_[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t count_ = 0;
};

}  // namespace aircannect
