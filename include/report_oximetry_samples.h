#pragma once

#include "report_oximetry.h"
#include "oximetry_archive.h"

namespace aircannect {

// Time is always UTC. Zero duration denotes an observation, not a held value.
struct ReportOximetrySample {
    int64_t time_ms = 0;
    uint32_t duration_ms = 0;
    enum { Oxygen, Pulse, Movement, Perfusion, Channels };
    float values[Channels] = {-1, -1, -1, -1};
};

class ReportOximetrySampleReader {
public:
    using Sink = bool (*)(void *, const ReportOximetrySample &);
    void begin(const ReportOximetryRecording &recording);
    bool consume(const uint8_t *bytes, size_t size, void *context, Sink sink);
    bool complete() const;

private:
    bool csv_row(ReportOximetrySample &sample);
    const ReportOximetryRecording *recording_ = nullptr;
    char row_[OximetryCsv::MaxHeader]{};
    size_t used_ = 0;
    uint32_t samples_ = 0;
    int64_t last_ms_ = 0;
    bool columns_ = false, comment_ = false;
};

}  // namespace aircannect
