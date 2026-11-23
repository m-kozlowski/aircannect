#include "report_oximetry.h"

#include <cstring>
#include <map>

namespace aircannect {
namespace {
// An empty value is conflicting evidence, never a last-observation-wins link.
using Links = std::map<std::string, std::string, std::less<>,
                       LargeAllocator<std::pair<const std::string, std::string>>>;

void link(Links &links, const char *key, const char *value) {
    if (!*key || !*value) return;
    auto it = links.lower_bound(key);
    if (it == links.end() || it->first != key) links.emplace_hint(it, key, value);
    else if (it->second != value) it->second.clear();
}

const char *lookup(const Links &links, const char *key) {
    const auto it = links.find(key);
    return it == links.end() ? "" : it->second.c_str();
}

const char *recorded_peer(const ReportOximetryRecording &r, char (&ble)[17]) {
    if (r.csv_bytes) return strcmp(r.source, "udp") ? r.source : "";
    return oximetry_ble_source(r.peer, ble) ? ble : "";
}
}  // namespace

void resolve_oximetry_devices(ReportOximetryCatalog &catalog) {
    Links peer_for_archive, peer_for_serial, serial_for_peer;
    for (const auto &r : catalog) {
        char ble[17];
        const char *peer = recorded_peer(r, ble);
        if (!r.csv_bytes) link(peer_for_archive, r.source, peer);
        link(peer_for_serial, r.serial, peer);
        link(serial_for_peer, peer, r.serial);
    }

    for (auto &r : catalog) {
        char ble[17];
        const char *peer = recorded_peer(r, ble);
        if (!r.csv_bytes && !*peer) peer = lookup(peer_for_archive, r.source);
        const char *serial = r.serial;
        if (!*serial && *peer) serial = lookup(serial_for_peer, peer);

        const char *id = *peer ? peer : r.source;
        if (*serial && !(r.csv_bytes && !strcmp(r.source, "udp"))) {
            const char *bound = lookup(peer_for_serial, serial);
            id = *bound && !strcmp(lookup(serial_for_peer, bound), serial) ? bound : serial;
        }
        strcpy(r.device, id);
    }
}
}  // namespace aircannect
