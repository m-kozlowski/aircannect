#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_clock.h"
#include "sleep_day_id.h"

namespace aircannect {

static constexpr const char *EDF_SESSION_METADATA_ROOT =
    "/aircannect/edf-sessions";

struct EdfSessionMetadata {
    SleepDayId raw_sleep_day;
    SleepDayId canonical_sleep_day;

    int64_t raw_segment_start_ms = 0;
    int64_t raw_segment_end_ms = 0;
    int64_t canonical_segment_start_ms = 0;
    int64_t canonical_segment_end_ms = 0;

    int64_t raw_therapy_start_ms = 0;
    int64_t raw_therapy_end_ms = 0;
    int64_t canonical_therapy_start_ms = 0;
    int64_t canonical_therapy_end_ms = 0;

    int64_t device_minus_utc_ms = 0;
    int32_t timezone_offset_minutes = 0;
    uint32_t capture_session_id = 0;

    char datalog_sleep_day[9] = {};
    char session_stamp[16] = {};

    bool externally_corrected = false;
    bool finalized = false;
};

struct EdfSessionMetadataFileInfo {
    uint16_t version = 0;
    uint16_t header_bytes = 0;
    uint32_t total_bytes = 0;
};

class EdfSessionMetadataCodec {
public:
    static constexpr uint16_t Version = 1;
    static constexpr size_t RecordBytes = 160;

    static bool encode(const EdfSessionMetadata &metadata,
                       uint8_t *out,
                       size_t out_size);
    static bool inspect(const uint8_t *bytes,
                        size_t length,
                        EdfSessionMetadataFileInfo &info);
    static bool decode(const uint8_t *bytes,
                       size_t length,
                       EdfSessionMetadata &metadata);
    static uint64_t identity(const uint8_t *bytes, size_t length);
};

bool edf_session_metadata_valid(const EdfSessionMetadata &metadata);
bool edf_session_metadata_begin(int64_t raw_segment_start_ms,
                                int64_t raw_therapy_start_ms,
                                const As11ClockTransform &clock,
                                int32_t timezone_offset_minutes,
                                uint32_t capture_session_id,
                                EdfSessionMetadata &metadata);
bool edf_session_metadata_finalize(EdfSessionMetadata &metadata,
                                   int64_t raw_segment_end_ms,
                                   int64_t raw_therapy_end_ms);
bool edf_session_metadata_path(const EdfSessionMetadata &metadata,
                               char *out,
                               size_t out_size);
bool edf_session_metadata_path_matches(const char *path,
                                       const EdfSessionMetadata &metadata);

}  // namespace aircannect
