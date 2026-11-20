#include "oximetry_archive.h"

#include <cstdio>
#include <charconv>
#include <ctime>
#include "utc_time.h"

namespace aircannect::OximetryCsv {

bool parse_header(std::string_view prefix, std::string_view source, std::string_view id,
                   Header &r) {
    const auto eol = prefix.find("\r\n");
    if (eol == prefix.npos) return false;
    auto row = prefix.substr(0, eol);

    auto take = [&](std::string_view token) {
        if (row.substr(0, token.size()) != token) return false;
        row.remove_prefix(token.size());
        return true;
    };
    if (!take("# Source=") || !take(source) || !take("/") || !take(id)) return false;
    if (id.substr(0, 5) == "live-" && !take(".csv")) return false;
    if (!take(" Version=2 OffsetMinutes=")) return false;

    int offset;
    const auto number = std::from_chars(row.data(), row.data() + row.size(), offset);
    if (number.ec != std::errc{} || offset < -1440 || offset > 1440) return false;
    row.remove_prefix(number.ptr - row.data());
    if (!take(" BLE=")) return false;
    const auto sn = row.find(" SN=");
    if (sn == row.npos || sn >= sizeof(r.peer)) return false;
    const auto peer = row.substr(0, sn);
    auto serial = row.substr(sn + 4);
    serial = serial.substr(0, serial.find_last_not_of(' ') + 1);
    if (!serial.empty() && !oximetry_source_valid(serial)) return false;

    memcpy(r.peer, peer.data(), peer.size());
    r.peer[peer.size()] = 0;
    memcpy(r.serial, serial.data(), serial.size());
    r.serial[serial.size()] = 0;
    r.offset_minutes = offset;
    return true;
}

size_t format_header(char (&out)[MaxHeader], const char *source, const char *id,
                     int32_t offset_minutes, const char *peer, const char *serial) {
    const int n = snprintf(out, sizeof(out),
        "# Source=%s/%s Version=2 OffsetMinutes=%ld BLE=%s SN=%-32s\r\n%s",
        source, id, long(offset_minutes), peer, serial, Columns);
    return n > 0 && size_t(n) < sizeof(out) ? size_t(n) : 0;
}

size_t format_row(char (&out)[MaxRow], int64_t utc_ms, int32_t offset_minutes, int oxygen, int pulse,
                  int motion, float pi_percent) {
    const time_t epoch = utc_ms / 1000 + int64_t(offset_minutes) * 60;
    tm local{};
    char time[32], utc[32], movement[6]{}, pi[12]{};
    if (!gmtime_r(&epoch, &local) ||
        !strftime(time, sizeof(time), "%H:%M:%S %b %d %Y", &local) ||
        !format_utc_iso8601_ms(utc_ms, utc, sizeof(utc))) return 0;

    if (motion >= 0) snprintf(movement, sizeof(movement), "%d", motion);
    if (pi_percent >= 0)
        snprintf(pi, sizeof(pi), "%.6g", double(pi_percent));

    const int n = snprintf(out, sizeof(out), "%s,%d,%d,%s,,,%s,%s\r\n",
                           time, oxygen, pulse, movement, utc, pi);
    return n > 0 && size_t(n) < sizeof(out) ? size_t(n) : 0;
}

}  // namespace aircannect::OximetryCsv
