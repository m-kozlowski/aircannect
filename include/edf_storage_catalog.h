#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_storage.h"
#include "edf_file_writer.h"

namespace aircannect {

struct EdfLocalDateTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

const char *edf_file_tag(EdfFileKind kind);
const char *edf_annotation_file_tag(EdfAnnotationKind kind);

bool edf_parse_as11_local_datetime(const char *text, EdfLocalDateTime &out);
bool edf_epoch_ms_to_local_datetime(int64_t epoch_ms,
                                    int32_t timezone_offset_minutes,
                                    EdfLocalDateTime &out);
bool edf_epoch_ms_to_configured_local_datetime(int64_t epoch_ms,
                                               EdfLocalDateTime &out);
bool edf_sleep_day_yyyymmdd(const EdfLocalDateTime &dt,
                            char *dst,
                            size_t dst_size);
bool edf_session_stamp(const EdfLocalDateTime &dt,
                       char *dst,
                       size_t dst_size);
bool edf_header_date(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size);
bool edf_header_time(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size);
bool edf_recording_id(const EdfLocalDateTime &dt,
                      const char *serial_number,
                      int32_t platform_id,
                      int32_t variant_id,
                      char *dst,
                      size_t dst_size);
bool edf_sleep_day_start(const EdfLocalDateTime &dt,
                         EdfLocalDateTime &start);
bool edf_sleep_day_epoch_days(const EdfLocalDateTime &dt,
                              uint16_t &days);
bool edf_sleep_day_minute(const EdfLocalDateTime &dt,
                          uint16_t &minute);
bool edf_datalog_dir(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size);
bool edf_datalog_path(EdfFileKind kind,
                      const EdfLocalDateTime &dt,
                      char *dst,
                      size_t dst_size);
bool edf_datalog_annotation_path(EdfAnnotationKind kind,
                                 const EdfLocalDateTime &dt,
                                 char *dst,
                                 size_t dst_size);
bool edf_str_path(char *dst, size_t dst_size);
bool edf_valid_browse_path(const char *path);
bool edf_valid_pull_path(const char *path);

}  // namespace aircannect
