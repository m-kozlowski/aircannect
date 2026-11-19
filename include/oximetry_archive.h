#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace aircannect {

inline constexpr char OximetryArchiveRoot[] = "/aircannect/oximetry";
inline std::atomic<uint32_t> oximetry_archive_revision{1};
inline std::atomic<uint32_t> oximetry_live_revision{0};
inline void oximetry_archive_published() { ++oximetry_archive_revision; }

inline bool oximetry_source_valid(std::string_view source) {
    return !source.empty() && source.size() <= 32 && source.find_first_not_of(
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") == source.npos;
}

namespace OximetryCsv {
inline constexpr char Columns[] =
    "Time,Oxygen Level,Pulse Rate,Motion,O2 Reminder,PR Reminder,UTC,PI\r\n";
inline constexpr size_t MaxRow = 112;
}

}  // namespace aircannect
