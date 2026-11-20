#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace aircannect {

inline constexpr char OximetryArchiveRoot[] = "/aircannect/oximetry";
inline constexpr char OximetryDayRevisionSuffix[] = ".rev";

inline bool oximetry_source_valid(std::string_view source) {
    return !source.empty() && source.size() <= 32 && source.find_first_not_of(
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") == source.npos;
}

enum class OximetryArchiveFile { Other, LiveCsv, RingBin, RingCsv };

inline bool oximetry_archive_path(std::string_view full, std::string_view &source,
                                  std::string_view &suffix) {
    constexpr size_t prefix = sizeof(OximetryArchiveRoot);
    if (full.size() <= prefix || full.substr(0, prefix - 1) != OximetryArchiveRoot ||
        full[prefix - 1] != '/') return false;
    const auto rest = full.substr(prefix);
    const auto slash = rest.find('/');
    if (slash == rest.npos || !oximetry_source_valid(source = rest.substr(0, slash))) return false;
    suffix = rest.substr(slash + 1);
    return true;
}

inline OximetryArchiveFile oximetry_recording_file(std::string_view source,
                                                   std::string_view suffix,
                                                   std::string_view &id) {
    if (suffix.size() == 49 && suffix.substr(8, 6) == "/live-" &&
        suffix.substr(45) == ".csv") {
        id = suffix.substr(9, 36);
        if ((source == "udp" || (source.size() == 16 && source.substr(0, 4) == "ble-" &&
             source.substr(4).find_first_not_of("0123456789abcdef") == source.npos)) &&
            id.substr(5, 8) == suffix.substr(0, 8) && id[13] == '-' && id[27] == '-' &&
            id.substr(5, 8).find_first_not_of("0123456789") == id.npos &&
            id.substr(14, 13).find_first_not_of("0123456789") == id.npos &&
            id.substr(28).find_first_not_of("0123456789abcdef") == id.npos)
            return OximetryArchiveFile::LiveCsv;
    }
    if (suffix.size() == 27 && suffix[8] == '/') {
        id = suffix.substr(9, 14);
        if (id.substr(0, 8) == suffix.substr(0, 8) && id.find_first_not_of("0123456789") == id.npos) {
            if (suffix.substr(23) == ".bin") return OximetryArchiveFile::RingBin;
            if (suffix.substr(23) == ".csv") return OximetryArchiveFile::RingCsv;
        }
    }
    return OximetryArchiveFile::Other;
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
