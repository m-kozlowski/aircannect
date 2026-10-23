#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr uint16_t AC_WIREGUARD_DEFAULT_PORT = 51820;
static constexpr uint16_t AC_WIREGUARD_DEFAULT_KEEPALIVE_S = 25;
static constexpr const char *AC_WIREGUARD_DEFAULT_ALLOWED_IPS = "0.0.0.0/0";
static constexpr size_t AC_WIREGUARD_KEY_TEXT_LEN = 44;
static constexpr size_t AC_WIREGUARD_HOST_MAX = 128;
static constexpr size_t AC_WIREGUARD_HOME_SSIDS_MAX = 160;
static constexpr size_t AC_WIREGUARD_ALLOWED_IPS_MAX = 160;

enum class WireguardMode : uint8_t {
    Off,
    Away,
    Always,
};

struct WireguardConfig {
    WireguardMode mode = WireguardMode::Off;
    // Comma-separated SSIDs where the tunnel stays down in Away mode.
    String home_ssids;
    // Tunnel IPv4 address; a trailing "/32" from a .conf is accepted.
    String address;
    String private_key;
    String peer_public_key;
    String preshared_key;
    // host[:port]; the port defaults to 51820.
    String endpoint;
    uint16_t keepalive_s = AC_WIREGUARD_DEFAULT_KEEPALIVE_S;
    // Comma-separated IPv4 CIDRs accepted from the peer.
    String allowed_ips = AC_WIREGUARD_DEFAULT_ALLOWED_IPS;
};

struct WireguardEndpoint {
    String host;
    uint16_t port = AC_WIREGUARD_DEFAULT_PORT;
};

struct WireguardCidr {
    char ip[16] = {};
    char netmask[16] = {};
};

const char *wireguard_mode_name(WireguardMode mode);
bool parse_wireguard_mode(const String &value, WireguardMode &mode);

// Trims and canonicalizes the text fields without validating them.
void normalize_wireguard_config(WireguardConfig &config);

// Returns nullptr when the config is usable or empty, else a short reason.
// A config with mode Off may be incomplete; other modes need every
// required field.
const char *wireguard_config_reject_reason(const WireguardConfig &config);

bool wireguard_config_complete(const WireguardConfig &config);
bool parse_wireguard_endpoint(const String &text, WireguardEndpoint &out);
bool parse_wireguard_address(const String &text, char *ip, size_t ip_size);

// Iterates the comma-separated CIDR list; returns false past the last entry
// or on a malformed entry (check `malformed`).
bool next_wireguard_cidr(const String &list,
                         size_t &cursor,
                         WireguardCidr &out,
                         bool &malformed);

bool wireguard_ssid_is_home(const WireguardConfig &config,
                            const String &ssid);

}  // namespace aircannect
