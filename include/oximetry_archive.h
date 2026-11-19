#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace aircannect {

inline constexpr char OximetryArchiveRoot[] = "/aircannect/oximetry";

inline bool oximetry_source_valid(std::string_view source) {
    return !source.empty() && source.size() <= 32 && source.find_first_not_of(
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") == source.npos;
}

namespace OximetryCsv {
inline constexpr char Columns[] =
    "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder,UTC,PI\r\n";
inline constexpr size_t MaxRow = 112;
constexpr size_t MaxHeader = 256;
size_t format_header(char (&out)[MaxHeader], const char *source, const char *id,
                     int32_t offset_minutes, const char *peer, const char *serial);
size_t format_row(char (&out)[MaxRow], int64_t utc_ms, int32_t offset_minutes, int oxygen, int pulse,
                  int motion, float pi_percent);
}

}  // namespace aircannect
