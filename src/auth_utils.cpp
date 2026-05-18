#include "auth_utils.h"

#include <stdlib.h>

namespace aircannect {

bool network_auth_required(const AppConfigData &cfg) {
    return cfg.http_user.length() || cfg.http_password.length();
}

uint32_t ip_to_u32(const IPAddress &ip) {
    return (static_cast<uint32_t>(ip[0]) << 24) |
           (static_cast<uint32_t>(ip[1]) << 16) |
           (static_cast<uint32_t>(ip[2]) << 8) |
           static_cast<uint32_t>(ip[3]);
}

namespace {

bool parse_ipv4(String text, uint32_t &out) {
    text.trim();
    IPAddress ip;
    if (!text.length() || !ip.fromString(text)) return false;
    out = ip_to_u32(ip);
    return true;
}

bool whitelist_entry_matches(String entry, uint32_t remote_ip) {
    entry.trim();
    if (!entry.length()) return false;
    if (entry == "*") return true;

    int slash = entry.indexOf('/');
    if (slash > 0) {
        uint32_t network = 0;
        if (!parse_ipv4(entry.substring(0, slash), network)) return false;
        String prefix_text = entry.substring(slash + 1);
        prefix_text.trim();
        char *end = nullptr;
        unsigned long prefix = strtoul(prefix_text.c_str(), &end, 10);
        if (!prefix_text.length() || *end || prefix > 32) return false;
        uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFUL << (32 - prefix));
        return (remote_ip & mask) == (network & mask);
    }

    int dash = entry.indexOf('-');
    if (dash > 0) {
        uint32_t first = 0;
        uint32_t last = 0;
        if (!parse_ipv4(entry.substring(0, dash), first) ||
            !parse_ipv4(entry.substring(dash + 1), last)) {
            return false;
        }
        if (first > last) {
            uint32_t tmp = first;
            first = last;
            last = tmp;
        }
        return remote_ip >= first && remote_ip <= last;
    }

    uint32_t exact = 0;
    return parse_ipv4(entry, exact) && remote_ip == exact;
}

}  // namespace

bool auth_whitelist_matches(const IPAddress &remote_ip,
                            const String &whitelist) {
    if (!whitelist.length()) return false;
    const uint32_t remote = ip_to_u32(remote_ip);
    int start = 0;
    while (start <= static_cast<int>(whitelist.length())) {
        int end = whitelist.indexOf(',', start);
        if (end < 0) end = whitelist.length();
        if (whitelist_entry_matches(whitelist.substring(start, end), remote)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

bool network_client_allowed(const AppConfigData &cfg,
                            const IPAddress &remote_ip) {
    if (!network_auth_required(cfg)) return true;
    return auth_whitelist_matches(remote_ip, cfg.auth_whitelist);
}

bool network_credentials_match(const AppConfigData &cfg,
                               const String &user,
                               const String &password) {
    return user == cfg.http_user && password == cfg.http_password;
}

}  // namespace aircannect
