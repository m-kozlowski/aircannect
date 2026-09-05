#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "report_artifact_key.h"

namespace aircannect {

struct ReportBuildTrackState {
    uint64_t valid_sample_count = 0;
    int64_t first_valid_sample_ms = 0;
    int64_t last_valid_sample_ms = 0;
    const uint8_t *tail_metrics = nullptr;
    size_t tail_metrics_size = 0;
};

struct ReportBuildCheckpoint {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    int64_t closed_before_ms = 0;

    const ReportBuildTrackState *tracks = nullptr;
    size_t track_count = 0;

    const uint8_t *progress = nullptr;
    size_t progress_size = 0;
    const uint8_t *metrics = nullptr;
    size_t metrics_size = 0;
    const uint8_t *events = nullptr;
    size_t events_size = 0;
};

struct ReportBuildCheckpointView {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    int64_t closed_before_ms = 0;

    const uint8_t *track_records = nullptr;
    size_t track_count = 0;

    const uint8_t *progress = nullptr;
    size_t progress_size = 0;
    const uint8_t *metrics = nullptr;
    size_t metrics_size = 0;
    const uint8_t *events = nullptr;
    size_t events_size = 0;

    bool track(size_t index, ReportBuildTrackState &track) const;
};

class ReportBuildCheckpointCodec {
public:
    static constexpr uint16_t Version = 1;
    static constexpr size_t HeaderBytes = 52;
    static constexpr size_t TrackBytes = 32;
    static constexpr size_t MaxBytes = 512 * 1024;

    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportBuildCheckpoint &checkpoint);
    static bool decode(const uint8_t *bytes,
                       size_t length,
                       ReportBuildCheckpointView &view);
};

bool report_build_checkpoint_path(SleepDayId sleep_day,
                                  uint32_t generation,
                                  uint8_t slot,
                                  char *out,
                                  size_t out_size);

}  // namespace aircannect
