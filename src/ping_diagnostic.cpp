#include "ping_diagnostic.h"

#include <algorithm>
#include <lwip/dns.h>
#include <lwip/tcpip.h>

namespace aircannect {

bool PingDiagnostic::start(const char *host, uint16_t count) {
    // Reject new check if previous one pending
    poll();
    const auto phase = phase_.load();
    if (phase != Phase::Idle && phase != Phase::Done) return false;

    if (!host || !*host || count == 0 || count > MaxCount) return false;

    result_ = {};
    count_ = count;
    cancelled_.store(false);
    phase_.store(Phase::Resolving);
    ip_addr_t address = {};

    LOCK_TCPIP_CORE();
    const err_t error = dns_gethostbyname_addrtype(
        host, &address, dns_done, this, LWIP_DNS_ADDRTYPE_IPV4);
    UNLOCK_TCPIP_CORE();

    if (error == ERR_OK) dns_done(host, &address, this);
    else if (error != ERR_INPROGRESS) fail("dns_failed");
    return true;
}

void PingDiagnostic::dns_done(const char *, const ip_addr_t *address, void *arg) {
    auto &self = *static_cast<PingDiagnostic *>(arg);
    if (self.cancelled_.load()) self.fail("cancelled");
    else if (!address || ip_addr_isany(address)) self.fail("dns_failed_or_invalid_ipv4");
    else {
        self.result_.address = *address;
        self.phase_.store(Phase::Ready);
    }
}

void PingDiagnostic::poll() {
    if (phase_.load() != Phase::Ready) return;
    if (cancelled_.load()) { fail("cancelled"); return; }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = result_.address;
    config.count = count_;
    config.data_size = 32;

    // SDK defaults: 1s timeout/interval, no bound interface (honors WG routes).
    esp_ping_callbacks_t callbacks = {};
    callbacks.cb_args = this;
    callbacks.on_ping_success = reply;
    callbacks.on_ping_timeout = timeout;
    callbacks.on_ping_end = done;
    esp_ping_handle_t handle = nullptr;
    phase_.store(Phase::Running);
    esp_err_t error = esp_ping_new_session(&config, &callbacks, &handle);
    if (error == ESP_OK) {
        error = esp_ping_start(handle);
        if (error != ESP_OK) esp_ping_delete_session(handle);
    }
    if (error != ESP_OK) fail(esp_err_to_name(error));
}

void PingDiagnostic::reply(esp_ping_handle_t handle, void *arg) {
    auto &self = *static_cast<PingDiagnostic *>(arg);
    uint32_t elapsed = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    self.result_.min_ms = std::min(self.result_.min_ms, elapsed);
    self.result_.max_ms = std::max(self.result_.max_ms, elapsed);
    self.result_.sum_ms += elapsed;
    timeout(handle, arg); // this will honor cancellation even after a successful reply.
}

void PingDiagnostic::timeout(esp_ping_handle_t handle, void *arg) {
    if (static_cast<PingDiagnostic *>(arg)->cancelled_.load()) esp_ping_stop(handle);
}

void PingDiagnostic::done(esp_ping_handle_t handle, void *arg) {
    auto &self = *static_cast<PingDiagnostic *>(arg);
    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &self.result_.sent, sizeof(self.result_.sent));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &self.result_.received, sizeof(self.result_.received));
    esp_ping_delete_session(handle);
    self.phase_.store(Phase::Done);
}

}  // namespace aircannect
