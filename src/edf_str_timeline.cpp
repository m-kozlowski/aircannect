#include "edf_str_timeline.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "calendar_utils.h"
#include "edf_bytes.h"
#include "edf_file_reader.h"
#include "edf_file_writer.h"
#include "edf_layout.h"
#include "edf_str_file_layout.h"
#include "edf_str_record_merge.h"

namespace aircannect {
namespace {

bool day_encodable(int64_t day) {
    return day >= 0 && day <= INT16_MAX;
}

bool format_header_date(int32_t epoch_day, char (&date)[9]) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(epoch_day, year, month, day) ||
        year < 1985 || year > 2084) {
        return false;
    }

    const int written = snprintf(date,
                                 sizeof(date),
                                 "%02u.%02u.%02u",
                                 day,
                                 month,
                                 static_cast<unsigned>(year % 100));
    return written == 8;
}

bool set_record_day(uint8_t *record, size_t record_size, int32_t day) {
    if (!record || record_size != edf_str_record_size() ||
        !day_encodable(day)) {
        return false;
    }

    const size_t date_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DATE_SIGNAL);
    const size_t crc_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_CRC_SIGNAL);
    if (date_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        crc_offset + 1 != AC_EDF_STR_SAMPLES_PER_RECORD) {
        return false;
    }

    edf_write_i16_le_sample(record,
                            date_offset,
                            static_cast<int16_t>(day));
    const uint16_t crc = edf_crc16_ccitt_false(record, crc_offset * 2);
    edf_write_i16_le_sample(record,
                            crc_offset,
                            static_cast<int16_t>(crc));
    return true;
}

bool render_filler_record(int32_t day, uint8_t *record, size_t record_size) {
    if (!record || record_size != edf_str_record_size()) return false;

    memset(record, 0xff, record_size);
    return set_record_day(record, record_size, day);
}

}  // namespace

bool edf_str_header_start_day(const uint8_t *header,
                              size_t header_size,
                              int32_t &day) {
    day = -1;
    EdfHeaderSummary summary;
    int64_t start_ms = 0;
    if (!edf_parse_header_summary(header, header_size, summary) ||
        !edf_parse_header_start_ms(summary, start_ms)) {
        return false;
    }

    const int64_t epoch_day = start_ms / 86400000LL;
    if (!day_encodable(epoch_day)) return false;
    day = static_cast<int32_t>(epoch_day);
    return true;
}

bool edf_str_patch_header_timeline(uint8_t *header,
                                   size_t header_size,
                                   int32_t start_day,
                                   uint32_t record_count) {
    if (!header || header_size != edf_str_header_size()) return false;

    char date[9] = {};
    char count[AC_EDF_HEADER_RECORD_COUNT_WIDTH] = {};
    if (!format_header_date(start_day, date) ||
        !edf_str_format_record_count_field(record_count,
                                           count,
                                           sizeof(count))) {
        return false;
    }

    memcpy(header + AC_EDF_HEADER_START_DATE_OFFSET,
           date,
           AC_EDF_HEADER_START_DATE_WIDTH);
    memcpy(header + AC_EDF_HEADER_RECORD_COUNT_OFFSET,
           count,
           AC_EDF_HEADER_RECORD_COUNT_WIDTH);
    return true;
}

bool edf_str_timeline_record_day(int32_t header_start_day,
                                 uint32_t record_index,
                                 int16_t date_sample,
                                 int32_t &day) {
    const int64_t expected =
        static_cast<int64_t>(header_start_day) + record_index;
    if (!day_encodable(expected)) return false;

    day = edf_str_date_sample_valid(date_sample)
              ? static_cast<int32_t>(date_sample)
              : static_cast<int32_t>(expected);
    return day_encodable(day);
}

bool edf_str_timeline_begin(int32_t header_start_day,
                            uint32_t record_count,
                            EdfStrTimelineScan &scan) {
    scan = {};
    if (!day_encodable(header_start_day)) return false;

    const int64_t expected_end = record_count == 0
                                     ? header_start_day
                                     : static_cast<int64_t>(header_start_day) +
                                           record_count - 1;
    if (!day_encodable(expected_end)) return false;

    scan.header_start_day = header_start_day;
    scan.record_count = record_count;
    scan.continuous = true;
    return true;
}

bool edf_str_timeline_scan_record(EdfStrTimelineScan &scan,
                                  uint32_t record_index,
                                  int16_t date_sample) {
    if (record_index != scan.scanned_records ||
        record_index >= scan.record_count) {
        return false;
    }

    int32_t day = -1;
    if (!edf_str_timeline_record_day(scan.header_start_day,
                                     record_index,
                                     date_sample,
                                     day)) {
        return false;
    }

    if (scan.minimum_record_day < 0 || day < scan.minimum_record_day) {
        scan.minimum_record_day = day;
    }
    if (scan.maximum_record_day < 0 || day > scan.maximum_record_day) {
        scan.maximum_record_day = day;
    }

    const int64_t expected =
        static_cast<int64_t>(scan.header_start_day) + record_index;
    if (!edf_str_date_sample_valid(date_sample) || day != expected) {
        scan.continuous = false;
    }
    scan.scanned_records++;
    return true;
}

bool edf_str_timeline_plan(const EdfStrTimelineScan &scan,
                           int16_t incoming_day,
                           bool force_rewrite,
                           EdfStrTimelinePlan &plan) {
    plan = {};
    if (!day_encodable(scan.header_start_day) ||
        !edf_str_date_sample_valid(incoming_day) ||
        scan.scanned_records != scan.record_count) {
        return false;
    }

    int64_t start_day = scan.header_start_day;
    int64_t end_day = scan.header_start_day;
    if (scan.record_count > 0) {
        const int64_t expected_end =
            static_cast<int64_t>(scan.header_start_day) +
            scan.record_count - 1;
        start_day = scan.minimum_record_day < start_day
                        ? scan.minimum_record_day
                        : start_day;
        end_day = scan.maximum_record_day > expected_end
                      ? scan.maximum_record_day
                      : expected_end;
    }
    if (incoming_day < start_day) start_day = incoming_day;
    if (incoming_day > end_day) end_day = incoming_day;
    if (!day_encodable(start_day) || !day_encodable(end_day) ||
        end_day < start_day) {
        return false;
    }

    const uint64_t full_count =
        static_cast<uint64_t>(end_day - start_day) + 1;
    const bool retain = scan.record_count > AC_EDF_STR_RECORD_LIMIT ||
                        full_count > AC_EDF_STR_RECORD_LIMIT;
    if (retain && full_count > AC_EDF_STR_RETAINED_RECORDS) {
        start_day = end_day - AC_EDF_STR_RETAINED_RECORDS + 1;
    }

    const uint64_t output_count =
        static_cast<uint64_t>(end_day - start_day) + 1;
    if (output_count == 0 || output_count > AC_EDF_STR_RECORD_LIMIT ||
        incoming_day < start_day) {
        return false;
    }

    plan.start_day = static_cast<int32_t>(start_day);
    plan.end_day = static_cast<int32_t>(end_day);
    plan.record_count = static_cast<uint32_t>(output_count);
    plan.incoming_index =
        static_cast<uint32_t>(incoming_day - plan.start_day);
    plan.retention_applied = retain;

    const int64_t existing_end =
        static_cast<int64_t>(scan.header_start_day) + scan.record_count;
    const bool direct_layout = scan.continuous && !force_rewrite && !retain;
    if (direct_layout && incoming_day >= scan.header_start_day &&
        incoming_day < existing_end) {
        plan.action = EdfStrTimelineAction::Replace;
    } else if (direct_layout && incoming_day == existing_end) {
        plan.action = EdfStrTimelineAction::Append;
    } else {
        plan.action = EdfStrTimelineAction::Rewrite;
    }
    return true;
}

bool edf_str_timeline_place_record(const EdfStrTimelinePlan &plan,
                                   EdfStrTimelineBuffer &buffer,
                                   int32_t day,
                                   uint8_t *record,
                                   size_t record_size,
                                   EdfStrTimelineBuildStats &stats) {
    if (!buffer.records || !buffer.present ||
        buffer.capacity < plan.record_count || !record ||
        record_size != edf_str_record_size()) {
        return false;
    }

    if (day < plan.start_day || day > plan.end_day) {
        stats.discarded_records++;
        return true;
    }

    const uint32_t index = static_cast<uint32_t>(day - plan.start_day);
    uint8_t *destination = buffer.records + index * record_size;
    if (buffer.present[index]) {
        const EdfStrRecordMergeStatus status =
            edf_str_merge_existing_record(destination,
                                          record_size,
                                          record,
                                          record_size);
        if (status != EdfStrRecordMergeStatus::Ok) return false;
        memcpy(destination, record, record_size);
        stats.merged_records++;
    } else {
        memcpy(destination, record, record_size);
        buffer.present[index] = 1;
        stats.placed_records++;
    }

    return set_record_day(destination, record_size, day);
}

bool edf_str_timeline_fill_missing(const EdfStrTimelinePlan &plan,
                                   EdfStrTimelineBuffer &buffer,
                                   EdfStrTimelineBuildStats &stats) {
    if (!buffer.records || !buffer.present ||
        buffer.capacity < plan.record_count) {
        return false;
    }

    const size_t record_size = edf_str_record_size();
    for (uint32_t i = 0; i < plan.record_count; ++i) {
        if (buffer.present[i]) continue;

        uint8_t *record = buffer.records + i * record_size;
        if (!render_filler_record(plan.start_day + static_cast<int32_t>(i),
                                  record,
                                  record_size)) {
            return false;
        }
        buffer.present[i] = 1;
        stats.filler_records++;
    }
    return true;
}

}  // namespace aircannect
