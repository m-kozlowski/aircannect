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

bool edf_parse_as11_local_datetime(const char *text, EdfLocalDateTime &out);
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
bool edf_datalog_dir(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size);
bool edf_datalog_path(EdfFileKind kind,
                      const EdfLocalDateTime &dt,
                      char *dst,
                      size_t dst_size);

}  // namespace aircannect
