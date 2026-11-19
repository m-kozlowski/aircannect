#include "oximetry_archive.h"

#include <cstdio>
#include <ctime>
#include "utc_time.h"

namespace aircannect::OximetryCsv {

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
