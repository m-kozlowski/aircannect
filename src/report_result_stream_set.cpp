#include "report_result_stream_set.h"

#include <string.h>

namespace aircannect {

void ReportResultStreamSet::clear() {
    memset(streams_, 0, sizeof(streams_));
    count_ = 0;
}

bool ReportResultStreamSet::add_or_update(ReportStoreChunkKind kind,
                                          ReportSourceId source,
                                          ReportSignalId signal,
                                          const char *name,
                                          bool required,
                                          bool complete,
                                          ReportResultStatus &status,
                                          size_t &stream_index) {
    if (!name || !name[0]) return false;

    for (size_t i = 0; i < count_; ++i) {
        ReportResultStream &stream = streams_[i];
        if (stream.kind != kind || stream.signal != signal ||
            !stream.name || strcmp(stream.name, name) != 0) {
            continue;
        }

        stream_index = i;
        if (required) stream.required = true;
        if (!complete && stream.complete) {
            stream.complete = false;
            if (stream.required) status.missing_streams++;
        }
        if (stream.chunk_count == 0) {
            stream.source = source;
        }
        return true;
    }

    if (full()) return false;

    stream_index = count_;
    ReportResultStream &stream = streams_[count_++];
    stream.kind = kind;
    stream.source = source;
    stream.signal = signal;
    stream.name = name;
    stream.required = required;
    stream.complete = complete;

    status.stream_count = static_cast<uint32_t>(count_);
    if (required && !complete) status.missing_streams++;
    return true;
}

}  // namespace aircannect
