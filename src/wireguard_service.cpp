#include "wireguard_service.h"

#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"

extern "C" {
#include "esp_wireguard.h"
}

namespace aircannect {

namespace {

static constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 30000;
static constexpr uint32_t RETRY_MIN_MS = 15000;
static constexpr uint32_t RETRY_MAX_MS = 5 * 60 * 1000;
static constexpr uint32_t CHECK_INTERVAL_MS = 1000;
// Reject a session whose peer has been silent this long; WireGuard rekeys
// every two minutes, so a healthy peer always handshakes well within it.
static constexpr int32_t HANDSHAKE_STALE_S = 300;
// Earliest wall clock treated as synced (2024-01-01T00:00:00Z).
static constexpr time_t MIN_VALID_EPOCH = 1704067200;

// The library keeps a single global tunnel; these mirror that.
wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();
wireguard_ctx_t wg_ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();

class LwipCoreGuard {
public:
    LwipCoreGuard()
        : locked_(!sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) {
        if (locked_) LOCK_TCPIP_CORE();
    }
    ~LwipCoreGuard() {
        if (locked_) UNLOCK_TCPIP_CORE();
    }
    LwipCoreGuard(const LwipCoreGuard &) = delete;
    LwipCoreGuard &operator=(const LwipCoreGuard &) = delete;

private:
    bool locked_;
};

bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

}  // namespace

const char *wireguard_state_name(WireguardState state) {
    switch (state) {
        case WireguardState::Disabled: return "disabled";
        case WireguardState::Home: return "home";
        case WireguardState::WaitingNetwork: return "waiting_network";
        case WireguardState::WaitingClock: return "waiting_clock";
        case WireguardState::Connecting: return "connecting";
        case WireguardState::Up: return "up";
        case WireguardState::Backoff: return "backoff";
    }
    return "unknown";
}

void WireguardService::configure(const WireguardConfig &config) {
    config_ = config;
    restart_pending_ = true;
    retry_delay_ms_ = 0;
    retry_at_ms_ = 0;
    set_error("");
    portENTER_CRITICAL(&status_lock_);
    status_.mode = config.mode;
    portEXIT_CRITICAL(&status_lock_);
}

bool WireguardService::wanted(bool sta_online, const String &ssid) const {
    if (config_.mode == WireguardMode::Off ||
        !wireguard_config_complete(config_)) {
        return false;
    }
    if (!sta_online) return false;
    if (config_.mode == WireguardMode::Away &&
        wireguard_ssid_is_home(config_, ssid)) {
        return false;
    }
    return true;
}

void WireguardService::poll(bool sta_online,
                            const String &ssid,
                            bool clock_valid,
                            uint32_t now_ms) {
    if (restart_pending_) {
        restart_pending_ = false;
        if (tunnel_open_) stop("config changed");
    }

    if (!wanted(sta_online, ssid)) {
        if (tunnel_open_) {
            stop(sta_online ? "home network" : "network down");
        }
        retry_delay_ms_ = 0;
        if (config_.mode == WireguardMode::Off ||
            !wireguard_config_complete(config_)) {
            set_state(WireguardState::Disabled);
        } else if (!sta_online) {
            set_state(WireguardState::WaitingNetwork);
        } else {
            set_state(WireguardState::Home);
        }
        return;
    }

    if (tunnel_open_) {
        if (!deadline_reached(now_ms, last_check_ms_ + CHECK_INTERVAL_MS)) {
            return;
        }
        last_check_ms_ = now_ms;
        if (peer_connected_) {
            poll_up(now_ms);
        } else {
            (void)continue_connect(now_ms);
        }
        return;
    }

    if (retry_at_ms_ && !deadline_reached(now_ms, retry_at_ms_)) return;

    // Handshake initiations carry a TAI64N timestamp; the peer drops any
    // that is older than one it has already seen.
    if (!clock_valid || time(nullptr) < MIN_VALID_EPOCH) {
        set_state(WireguardState::WaitingClock);
        return;
    }

    (void)start(now_ms);
}

bool WireguardService::start(uint32_t now_ms) {
    WireguardEndpoint endpoint;
    char address[16] = {};
    if (!parse_wireguard_endpoint(config_.endpoint, endpoint) ||
        !parse_wireguard_address(config_.address, address, sizeof(address))) {
        schedule_retry(now_ms, "invalid config");
        return false;
    }

    address_ = address;
    endpoint_host_ = endpoint.host;

    wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();
    wg_config.private_key = config_.private_key.c_str();
    wg_config.public_key = config_.peer_public_key.c_str();
    wg_config.preshared_key = config_.preshared_key.length()
                                  ? config_.preshared_key.c_str()
                                  : nullptr;
    wg_config.address = address_.c_str();
    wg_config.netmask = "255.255.255.255";
    wg_config.endpoint = endpoint_host_.c_str();
    wg_config.port = endpoint.port;
    wg_config.persistent_keepalive = config_.keepalive_s;
    wg_ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();

    esp_err_t err = ESP_FAIL;
    {
        LwipCoreGuard guard;
        err = esp_wireguard_init(&wg_config, &wg_ctx);
    }
    if (err != ESP_OK) {
        Log::logf(CAT_WIFI, LOG_WARN, "[WG] init failed: %s\n",
                  esp_err_to_name(err));
        schedule_retry(now_ms, "init failed");
        return false;
    }

    tunnel_open_ = true;
    peer_connected_ = false;
    connect_started_ms_ = now_ms;
    last_check_ms_ = now_ms;
    set_state(WireguardState::Connecting);
    Log::logf(CAT_WIFI, LOG_INFO, "[WG] connecting to %s:%u\n",
              endpoint_host_.c_str(), static_cast<unsigned>(endpoint.port));
    return continue_connect(now_ms);
}

bool WireguardService::continue_connect(uint32_t now_ms) {
    if (!wg_ctx.netif || !peer_connected_) {
        esp_err_t err = ESP_FAIL;
        {
            LwipCoreGuard guard;
            err = esp_wireguard_connect(&wg_ctx);
        }
        if (err == ESP_ERR_RETRY) {
            if (deadline_reached(now_ms,
                                 connect_started_ms_ + HANDSHAKE_TIMEOUT_MS)) {
                stop("endpoint lookup timed out");
                schedule_retry(now_ms, "endpoint lookup failed");
                return false;
            }
            return true;
        }
        if (err != ESP_OK) {
            Log::logf(CAT_WIFI, LOG_WARN, "[WG] connect failed: %s\n",
                      esp_err_to_name(err));
            stop("connect failed");
            schedule_retry(now_ms, "connect failed");
            return false;
        }

        size_t cursor = 0;
        WireguardCidr cidr;
        bool malformed = false;
        LwipCoreGuard guard;
        while (next_wireguard_cidr(config_.allowed_ips, cursor, cidr,
                                   malformed)) {
            if (esp_wireguard_add_allowed_ip(&wg_ctx, cidr.ip,
                                             cidr.netmask) != ESP_OK) {
                Log::logf(CAT_WIFI, LOG_WARN,
                          "[WG] allowed IP rejected: %s/%s\n", cidr.ip,
                          cidr.netmask);
            }
        }
        peer_connected_ = true;
    }
    return true;
}

void WireguardService::poll_up(uint32_t now_ms) {
    bool peer_up = false;
    time_t handshake = 0;
    bool default_route = false;
    {
        LwipCoreGuard guard;
        peer_up = esp_wireguard_peer_is_up(&wg_ctx) == ESP_OK;
        if (esp_wireguard_latest_handshake(&wg_ctx, &handshake) != ESP_OK) {
            handshake = 0;
        }
        // Route everything through the tunnel only once the peer answers,
        // so a dead endpoint does not black-hole the device's traffic.
        // DHCP renewals can reset the default netif, so reassert it.
        if (peer_up && netif_default != wg_ctx.netif) {
            esp_wireguard_set_default(&wg_ctx);
        } else if (!peer_up && netif_default == wg_ctx.netif) {
            esp_wireguard_restore_default(&wg_ctx);
        }
        default_route = netif_default == wg_ctx.netif;
    }

    int32_t age_s = -1;
    if (handshake > 0) {
        const time_t now = time(nullptr);
        age_s = now >= handshake ? static_cast<int32_t>(now - handshake) : 0;
    }

    portENTER_CRITICAL(&status_lock_);
    const bool was_up = status_.state == WireguardState::Up;
    status_.handshake_age_s = age_s;
    status_.default_route = default_route;
    portEXIT_CRITICAL(&status_lock_);

    if (peer_up) {
        if (!was_up) {
            Log::logf(CAT_WIFI, LOG_INFO, "[WG] tunnel up via %s\n",
                      endpoint_host_.c_str());
            portENTER_CRITICAL(&status_lock_);
            status_.connects++;
            portEXIT_CRITICAL(&status_lock_);
            retry_delay_ms_ = 0;
            set_error("");
        }
        set_state(WireguardState::Up);
        if (age_s > HANDSHAKE_STALE_S) {
            stop("handshake stale");
            schedule_retry(now_ms, "peer stopped responding");
        }
        return;
    }

    if (was_up) {
        Log::logf(CAT_WIFI, LOG_WARN, "[WG] peer down\n");
        connect_started_ms_ = now_ms;
        set_state(WireguardState::Connecting);
    }
    if (deadline_reached(now_ms, connect_started_ms_ + HANDSHAKE_TIMEOUT_MS)) {
        stop("no handshake");
        schedule_retry(now_ms, "no handshake from peer");
    }
}

void WireguardService::stop(const char *reason) {
    if (!tunnel_open_) return;
    {
        LwipCoreGuard guard;
        if (wg_ctx.netif) {
            esp_wireguard_disconnect(&wg_ctx);
        } else {
            esp_wireguard_restore_default(&wg_ctx);
        }
    }
    tunnel_open_ = false;
    peer_connected_ = false;
    portENTER_CRITICAL(&status_lock_);
    status_.default_route = false;
    status_.handshake_age_s = -1;
    portEXIT_CRITICAL(&status_lock_);
    Log::logf(CAT_WIFI, LOG_INFO, "[WG] tunnel stopped: %s\n",
              reason ? reason : "");
}

void WireguardService::schedule_retry(uint32_t now_ms, const char *error) {
    retry_delay_ms_ = retry_delay_ms_ == 0
                          ? RETRY_MIN_MS
                          : min(retry_delay_ms_ * 2, RETRY_MAX_MS);
    retry_at_ms_ = now_ms + retry_delay_ms_;
    if (retry_at_ms_ == 0) retry_at_ms_ = 1;
    set_error(error);
    set_state(WireguardState::Backoff);
    Log::logf(CAT_WIFI, LOG_WARN, "[WG] %s; retry in %lu s\n", error,
              static_cast<unsigned long>(retry_delay_ms_ / 1000));
}

void WireguardService::set_state(WireguardState state) {
    portENTER_CRITICAL(&status_lock_);
    status_.state = state;
    portEXIT_CRITICAL(&status_lock_);
}

void WireguardService::set_error(const char *error) {
    portENTER_CRITICAL(&status_lock_);
    strlcpy(status_.error, error ? error : "", sizeof(status_.error));
    portEXIT_CRITICAL(&status_lock_);
}

WireguardStatus WireguardService::status() const {
    portENTER_CRITICAL(&status_lock_);
    const WireguardStatus out = status_;
    portEXIT_CRITICAL(&status_lock_);
    return out;
}

}  // namespace aircannect
