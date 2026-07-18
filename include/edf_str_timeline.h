#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

// STR Date samples and timeline fields are Unix epoch day numbers.
static constexpr uint32_t AC_EDF_STR_RECORD_LIMIT = 547;
static constexpr uint32_t AC_EDF_STR_RETAINED_RECORDS = 360;

enum class EdfStrTimelineAction : uint8_t {
    Replace,
    Append,
    Rewrite,
};

struct EdfStrTimelineScan {
    int32_t header_start_day = -1;
    int32_t minimum_record_day = -1;
    int32_t maximum_record_day = -1;
    uint32_t record_count = 0;
    uint32_t scanned_records = 0;
    bool continuous = false;
};

struct EdfStrTimelinePlan {
    EdfStrTimelineAction action = EdfStrTimelineAction::Rewrite;
    int32_t start_day = -1;
    int32_t end_day = -1;
    uint32_t record_count = 0;
    uint32_t incoming_index = 0;
    bool retention_applied = false;
};

struct EdfStrTimelineBuffer {
    uint8_t *records = nullptr;
    uint8_t *present = nullptr;
    uint32_t capacity = 0;
};

struct EdfStrTimelineBuildStats {
    uint32_t placed_records = 0;
    uint32_t merged_records = 0;
    uint32_t discarded_records = 0;
    uint32_t filler_records = 0;
};

bool edf_str_header_start_day(const uint8_t *header,
                              size_t header_size,
                              int32_t &day);
bool edf_str_patch_header_timeline(uint8_t *header,
                                   size_t header_size,
                                   int32_t start_day,
                                   uint32_t record_count);

bool edf_str_timeline_begin(int32_t header_start_day,
                            uint32_t record_count,
                            EdfStrTimelineScan &scan);
bool edf_str_timeline_scan_record(EdfStrTimelineScan &scan,
                                  uint32_t record_index,
                                  int16_t date_sample);
bool edf_str_timeline_plan(const EdfStrTimelineScan &scan,
                           int16_t incoming_day,
                           bool force_rewrite,
                           EdfStrTimelinePlan &plan);
bool edf_str_timeline_record_day(int32_t header_start_day,
                                 uint32_t record_index,
                                 int16_t date_sample,
                                 int32_t &day);

bool edf_str_timeline_place_record(const EdfStrTimelinePlan &plan,
                                   EdfStrTimelineBuffer &buffer,
                                   int32_t day,
                                   uint8_t *record,
                                   size_t record_size,
                                   EdfStrTimelineBuildStats &stats);
bool edf_str_timeline_fill_missing(const EdfStrTimelinePlan &plan,
                                   EdfStrTimelineBuffer &buffer,
                                   EdfStrTimelineBuildStats &stats);

}  // namespace aircannect
