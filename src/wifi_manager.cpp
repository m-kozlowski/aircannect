#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "board.h"
#include "debug_log.h"

namespace aircannect {

namespace {

static constexpr const char *WIFI_PREF_NS = "wifi";
static constexpr const char *WIFI_PREF_HAS = "has";
static constexpr const char *WIFI_PREF_SSID = "ssid";
static constexpr const char *WIFI_PREF_PASS = "pass";
static constexpr const char *WIFI_PREF_OPEN = "open";
static constexpr const char *WIFI_PREF_COUNT = "count";

static volatile uint8_t last_disconnect_reason = 0;

void format_bssid(char *out, size_t size, const uint8_t *bssid) {
    if (!out || size == 0) return;
    if (!bssid) {
        out[0] = 0;
        return;
    }
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

String ap_ssid(const String &hostname) {
    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    String out = hostname.length() ? hostname : String(AC_DEV_SOFTAP_SSID);
    out += "_";
    out += suffix;
    return out;
}

void wifi_event_cb(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        last_disconnect_reason = info.wifi_sta_disconnected.reason;
    }
}

void remove_key_if_present(Preferences &prefs, const char *key) {
    if (prefs.isKey(key)) prefs.remove(key);
}

}  // namespace

bool WifiManager::begin() {
    load_config();
    stop_wifi();
    WiFi.onEvent(wifi_event_cb);

    if (softap_mode_ == SoftApMode::Forced) {
        start_softap(sta_configured_);
    }

    if (sta_configured_) return start_next_profile(0);

#if defined(AC_WIFI_STA_SSID) && defined(AC_WIFI_STA_PASS)
    if (!sta_configured_) {
        if (add_profile(String(AC_WIFI_STA_SSID),
                        String(AC_WIFI_STA_PASS),
                        false)) {
            return start_next_profile(0);
        }
    }
#endif

    network_available_ = start_softap(false);
    return network_available_;
}

void WifiManager::poll() {
    cleanup_manual_scan();

    if (mode_state_ != WifiModeState::StaConnecting &&
        mode_state_ != WifiModeState::StaPmfRetry &&
        mode_state_ != WifiModeState::StaRoamScanning &&
        mode_state_ != WifiModeState::StaConnected) {
        return;
    }

    if (mode_state_ == WifiModeState::StaRoamScanning) {
        handle_roam_scan();
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (mode_state_ != WifiModeState::StaConnected) {
            handle_connected();
        }
        if (mode_state_ == WifiModeState::StaConnected) {
            maybe_start_roam_scan();
        }
        return;
    }

    if (mode_state_ == WifiModeState::StaConnected) {
        network_available_ = softap_running_;
        stats_.disconnects++;
        Log::logf(CAT_WIFI, LOG_WARN,
                  "[WiFi] STA disconnected; reconnecting\n");
        if (active_profile_index_ >= 0) {
            start_profile(static_cast<size_t>(active_profile_index_));
        } else {
            start_next_profile(0);
        }
        return;
    }

    if (static_cast<int32_t>(millis() - connect_deadline_ms_) >= 0) {
        handle_connect_timeout();
    }
}

void WifiManager::load_config() {
    Preferences prefs;
    if (!prefs.begin(WIFI_PREF_NS, true)) {
        sta_configured_ = false;
        return;
    }

    bool needs_save = false;
    profile_count_ = prefs.getUInt(WIFI_PREF_COUNT, 0);
    if (profile_count_ > AC_WIFI_PROFILE_MAX) {
        profile_count_ = AC_WIFI_PROFILE_MAX;
        needs_save = true;
    }
    for (size_t i = 0; i < profile_count_; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "ssid_%u", static_cast<unsigned>(i));
        profiles_[i].ssid = prefs.getString(key, "");
        snprintf(key, sizeof(key), "pass_%u", static_cast<unsigned>(i));
        profiles_[i].password = prefs.getString(key, "");
    }

    if (profile_count_ == 0 && prefs.getBool(WIFI_PREF_HAS, false)) {
        profiles_[0].ssid = prefs.getString(WIFI_PREF_SSID, "");
        const bool legacy_open = prefs.getBool(WIFI_PREF_OPEN, false);
        profiles_[0].password =
            legacy_open ? "" : prefs.getString(WIFI_PREF_PASS, "");
        if (profiles_[0].ssid.length()) profile_count_ = 1;
        needs_save = true;
    }
    prefs.end();

    size_t write = 0;
    const size_t loaded_count = profile_count_;
    for (size_t read = 0; read < profile_count_; ++read) {
        profiles_[read].ssid.trim();
        if (!profiles_[read].ssid.length()) {
            needs_save = true;
            continue;
        }
        if (write != read) {
            profiles_[write] = profiles_[read];
            needs_save = true;
        }
        write++;
    }
    for (size_t i = write; i < AC_WIFI_PROFILE_MAX; ++i) profiles_[i] = {};
    if (write != loaded_count) needs_save = true;
    profile_count_ = write;
    sta_configured_ = profile_count_ > 0;
    if (sta_configured_) {
        sta_ssid_ = profiles_[0].ssid;
        sta_pass_ = profiles_[0].password;
    } else {
        sta_configured_ = false;
        sta_ssid_ = "";
        sta_pass_ = "";
    }
    if (needs_save) save_config();
}

void WifiManager::save_config(size_t first_dirty_index) {
    Preferences prefs;
    if (!prefs.begin(WIFI_PREF_NS, false)) {
        Log::logf(CAT_WIFI, LOG_ERROR, "[WiFi] failed to open NVS namespace\n");
        return;
    }
    prefs.putBool(WIFI_PREF_HAS, sta_configured_);
    prefs.putUInt(WIFI_PREF_COUNT, profile_count_);
    if (first_dirty_index > AC_WIFI_PROFILE_MAX) first_dirty_index = 0;
    for (size_t i = first_dirty_index; i < AC_WIFI_PROFILE_MAX; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "ssid_%u", static_cast<unsigned>(i));
        if (i < profile_count_) prefs.putString(key, profiles_[i].ssid);
        else remove_key_if_present(prefs, key);
        snprintf(key, sizeof(key), "pass_%u", static_cast<unsigned>(i));
        if (i < profile_count_) prefs.putString(key, profiles_[i].password);
        else remove_key_if_present(prefs, key);
        snprintf(key, sizeof(key), "open_%u", static_cast<unsigned>(i));
        remove_key_if_present(prefs, key);
        snprintf(key, sizeof(key), "ena_%u", static_cast<unsigned>(i));
        remove_key_if_present(prefs, key);
    }
    if (first_dirty_index == 0) {
        prefs.putString(WIFI_PREF_SSID, profile_count_ ? profiles_[0].ssid : "");
        prefs.putString(WIFI_PREF_PASS,
                        profile_count_ ? profiles_[0].password : "");
        remove_key_if_present(prefs, WIFI_PREF_OPEN);
    }
    prefs.end();
}

bool WifiManager::start_profile(size_t index) {
    if (index >= profile_count_) return false;
    WifiProfile &profile = profiles_[index];
    if (!profile.ssid.length()) return false;

    if (softap_mode_ == SoftApMode::Forced && !softap_running_) {
        start_softap(true);
    }
    WiFi.mode(softap_mode_ == SoftApMode::Forced ? WIFI_AP_STA : WIFI_STA);
    apply_country_code();
    WiFi.setHostname(hostname_.c_str());
    mode_state_ = WifiModeState::StaConnecting;
    network_available_ = softap_running_;
    roam_connect_pending_ = false;
    active_profile_index_ = static_cast<int8_t>(index);
    sta_ssid_ = profile.ssid;
    sta_pass_ = profile.password;
    connect_deadline_ms_ = millis() + AC_WIFI_CONNECT_TIMEOUT_MS;
    pmf_retry_attempted_ = false;
    last_disconnect_reason_ = 0;
    last_disconnect_reason = 0;
    stats_.connect_attempts++;

    Log::logf(CAT_WIFI, LOG_INFO, "[WiFi] connecting to profile %u SSID=%s\n",
              static_cast<unsigned>(index), profile.ssid.c_str());
    if (!profile.password.length()) {
        WiFi.begin(profile.ssid.c_str());
    } else {
        WiFi.begin(profile.ssid.c_str(), profile.password.c_str());
    }
    return true;
}

bool WifiManager::start_next_profile(size_t start_index) {
    if (profile_count_ == 0) return false;
    for (size_t offset = 0; offset < profile_count_; ++offset) {
        const size_t index = (start_index + offset) % profile_count_;
        if (start_profile(index)) return true;
    }
    return false;
}

bool WifiManager::start_softap(bool with_sta) {
    WiFi.mode(with_sta ? WIFI_AP_STA : WIFI_AP);
    apply_country_code();
    const String ssid = ap_ssid(hostname_);
    bool ok = WiFi.softAP(ssid.c_str(), AC_DEV_SOFTAP_PASS);
    if (!ok) {
        mode_state_ = WifiModeState::Failed;
        Log::logf(CAT_WIFI, LOG_ERROR, "[WiFi] SoftAP start failed\n");
        return false;
    }
    softap_running_ = true;
    network_available_ = true;
    if (!with_sta) {
        mode_state_ = WifiModeState::SoftAp;
        active_profile_index_ = -1;
    }
    Log::logf(CAT_WIFI, LOG_WARN,
              "[WiFi] SoftAP %s SSID=%s IP=%s\n",
              softap_mode_name(softap_mode_),
              ssid.c_str(), WiFi.softAPIP().toString().c_str());
    return true;
}

void WifiManager::stop_wifi() {
    if (manual_scan_active_) {
        esp_wifi_scan_stop();
        WiFi.scanDelete();
        manual_scan_active_ = false;
        manual_scan_completed_ms_ = 0;
    }
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        WiFi.disconnect(true);
    }
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        WiFi.softAPdisconnect(true);
    }
    WiFi.mode(WIFI_OFF);
    network_available_ = false;
    softap_running_ = false;
    pmf_retry_attempted_ = false;
    roam_connect_pending_ = false;
    active_profile_index_ = -1;
    last_disconnect_reason_ = 0;
    last_disconnect_reason = 0;
    mode_state_ = WifiModeState::Off;
}

bool WifiManager::configure_sta(const String &ssid, const String &password) {
    if (!ssid.length()) return false;
    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) profiles_[i] = {};
    profiles_[0].ssid = ssid;
    profiles_[0].password = password;
    profile_count_ = 1;
    sta_configured_ = true;
    save_config();
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] configured single STA profile SSID=%s auth=%s\n",
              ssid.c_str(), password.length() ? "password" : "open");
    return reconnect();
}

void WifiManager::set_hostname(const String &hostname) {
    if (!hostname.length()) return;
    hostname_ = hostname;
    WiFi.setHostname(hostname_.c_str());
}

void WifiManager::set_softap_mode(SoftApMode mode) {
    softap_mode_ = mode;
}

void WifiManager::apply_softap_mode() {
    if (softap_mode_ == SoftApMode::Forced) {
        const bool with_sta =
            sta_configured_ || WiFi.status() == WL_CONNECTED ||
            mode_state_ == WifiModeState::StaConnecting ||
            mode_state_ == WifiModeState::StaPmfRetry ||
            mode_state_ == WifiModeState::StaRoamScanning;
        if (!softap_running_) {
            start_softap(with_sta);
        } else if (with_sta) {
            WiFi.mode(WIFI_AP_STA);
        }
        network_available_ = true;
        return;
    }

    if (!softap_running_ || WiFi.status() != WL_CONNECTED) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    softap_running_ = false;
    network_available_ = true;
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] SoftAP stopped after mode switch to auto\n");
}

void WifiManager::set_country_code(const String &country) {
    country_code_ = country;
    country_code_.trim();
    country_code_.toUpperCase();
}

void WifiManager::set_roaming_suspended(bool suspended) {
    roaming_suspended_ = suspended;
    if (!suspended) return;
    low_rssi_count_ = 0;
    if (mode_state_ == WifiModeState::StaRoamScanning) {
        esp_wifi_scan_stop();
        WiFi.scanDelete();
        mode_state_ = WiFi.status() == WL_CONNECTED
                          ? WifiModeState::StaConnected
                          : WifiModeState::StaConnecting;
    }
}

bool WifiManager::configure_open_sta(const String &ssid) {
    if (!ssid.length()) return false;
    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) profiles_[i] = {};
    profiles_[0].ssid = ssid;
    profiles_[0].password = "";
    profile_count_ = 1;
    sta_configured_ = true;
    save_config();
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] configured single open STA profile SSID=%s\n",
              ssid.c_str());
    return reconnect();
}

bool WifiManager::add_profile(const String &ssid, const String &password,
                              bool open_network) {
    String clean_ssid = ssid;
    clean_ssid.trim();
    if (!clean_ssid.length()) return false;

    for (size_t i = 0; i < profile_count_; ++i) {
        if (!profiles_[i].ssid.equals(clean_ssid)) continue;
        profiles_[i].password = open_network ? "" : password;
        sta_configured_ = true;
        save_config(i);
        Log::logf(CAT_WIFI, LOG_INFO,
                  "[WiFi] updated STA profile %u SSID=%s auth=%s\n",
                  static_cast<unsigned>(i), clean_ssid.c_str(),
                  open_network ? "open" : "password");
        return reconnect();
    }

    if (profile_count_ >= AC_WIFI_PROFILE_MAX) {
        Log::logf(CAT_WIFI, LOG_WARN,
                  "[WiFi] cannot add STA profile SSID=%s: profile list full\n",
                  clean_ssid.c_str());
        return false;
    }
    const size_t index = profile_count_;
    profiles_[index].ssid = clean_ssid;
    profiles_[index].password = open_network ? "" : password;
    profile_count_++;
    sta_configured_ = true;
    save_config(index);
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] added STA profile %u SSID=%s auth=%s\n",
              static_cast<unsigned>(index), clean_ssid.c_str(),
              open_network ? "open" : "password");
    return reconnect();
}

bool WifiManager::replace_profiles(const WifiProfile *profiles,
                                   size_t count,
                                   bool reconnect_now) {
    if (count > AC_WIFI_PROFILE_MAX) {
        Log::logf(CAT_WIFI, LOG_WARN,
                  "[WiFi] profile replace rejected: count=%u max=%u\n",
                  static_cast<unsigned>(count),
                  static_cast<unsigned>(AC_WIFI_PROFILE_MAX));
        return false;
    }
    if (count > 0 && !profiles) return false;

    WifiProfile cleaned[AC_WIFI_PROFILE_MAX];
    size_t write = 0;
    for (size_t i = 0; i < count; ++i) {
        String ssid = profiles[i].ssid;
        ssid.trim();
        if (!ssid.length()) continue;

        cleaned[write].ssid = ssid;
        cleaned[write].password = profiles[i].password;
        write++;
    }

    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) profiles_[i] = {};
    for (size_t i = 0; i < write; ++i) profiles_[i] = cleaned[i];
    profile_count_ = write;
    sta_configured_ = profile_count_ > 0;
    active_profile_index_ = -1;
    if (sta_configured_) {
        sta_ssid_ = profiles_[0].ssid;
        sta_pass_ = profiles_[0].password;
    } else {
        sta_ssid_ = "";
        sta_pass_ = "";
    }
    save_config();
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] replaced STA profiles count=%u reconnect=%s\n",
              static_cast<unsigned>(profile_count_),
              reconnect_now ? "yes" : "no");
    return reconnect_now ? reconnect() : true;
}

bool WifiManager::remove_profile(size_t index) {
    if (index >= profile_count_) return false;
    const String removed_ssid = profiles_[index].ssid;
    for (size_t i = index + 1; i < profile_count_; ++i) {
        profiles_[i - 1] = profiles_[i];
    }
    profile_count_--;
    profiles_[profile_count_] = {};
    sta_configured_ = profile_count_ > 0;
    save_config(index);
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] removed STA profile %u SSID=%s remaining=%u\n",
              static_cast<unsigned>(index), removed_ssid.c_str(),
              static_cast<unsigned>(profile_count_));
    return reconnect();
}

void WifiManager::clear_sta_config() {
    sta_configured_ = false;
    sta_ssid_ = "";
    sta_pass_ = "";
    profile_count_ = 0;
    active_profile_index_ = -1;
    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) profiles_[i] = {};
    save_config();
    Log::logf(CAT_WIFI, LOG_INFO, "[WiFi] cleared STA profiles\n");
    reconnect();
}

bool WifiManager::reconnect() {
    stop_wifi();
    consecutive_profile_failures_ = 0;
    if (softap_mode_ == SoftApMode::Forced) {
        start_softap(sta_configured_);
    }
    if (sta_configured_) return start_next_profile(0);

    network_available_ = start_softap(false);
    return network_available_;
}

IPAddress WifiManager::ip() const {
    if (mode_state_ == WifiModeState::SoftAp ||
        (softap_running_ && WiFi.status() != WL_CONNECTED)) {
        return WiFi.softAPIP();
    }
    return WiFi.localIP();
}

IPAddress WifiManager::gateway() const {
    if (mode_state_ == WifiModeState::SoftAp ||
        (softap_running_ && WiFi.status() != WL_CONNECTED)) {
        return WiFi.softAPIP();
    }
    return WiFi.gatewayIP();
}

IPAddress WifiManager::softap_ip() const {
    return WiFi.softAPIP();
}

int32_t WifiManager::rssi() const {
    if (mode_state_ != WifiModeState::StaConnected &&
        mode_state_ != WifiModeState::StaRoamScanning) {
        return 0;
    }
    return WiFi.RSSI();
}

int32_t WifiManager::channel() const {
    if (mode_state_ != WifiModeState::StaConnected &&
        mode_state_ != WifiModeState::StaRoamScanning) {
        return 0;
    }
    return WiFi.channel();
}

void WifiManager::bssid(char *out, size_t size) const {
    if (!out || size == 0) return;
    out[0] = 0;
    if (mode_state_ != WifiModeState::StaConnected &&
        mode_state_ != WifiModeState::StaRoamScanning) {
        return;
    }
    format_bssid(out, size, WiFi.BSSID());
}

uint32_t WifiManager::connect_timeout_remaining_ms() const {
    const int32_t remaining =
        static_cast<int32_t>(connect_deadline_ms_ - millis());
    return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
}

const char *WifiManager::state_name() const {
    return mode_name();
}

void WifiManager::apply_country_code() {
    if (country_code_.length() < 2) return;
    esp_err_t err = esp_wifi_set_country_code(country_code_.c_str(), true);
    if (err == ESP_OK) {
        Log::logf(CAT_WIFI, LOG_INFO, "[WiFi] country code=%s\n",
                  country_code_.c_str());
    } else {
        Log::logf(CAT_WIFI, LOG_WARN,
                  "[WiFi] country code %s rejected err=%d\n",
                  country_code_.c_str(), static_cast<int>(err));
    }
}

void WifiManager::handle_connected() {
    mode_state_ = WifiModeState::StaConnected;
    network_available_ = true;
    consecutive_profile_failures_ = 0;
    last_disconnect_reason_ = 0;
    last_disconnect_reason = 0;
    low_rssi_count_ = 0;
    last_roam_check_ms_ = millis();
    if (roam_connect_pending_) {
        stats_.roam_switches++;
        roam_connect_pending_ = false;
    }
    stats_.connect_successes++;
    Log::logf(CAT_WIFI, LOG_INFO, "[WiFi] STA connected, IP=%s\n",
              WiFi.localIP().toString().c_str());
}

void WifiManager::maybe_start_roam_scan() {
    if (!roaming_enabled() || roaming_suspended_) return;
    if (active_profile_index_ < 0) return;
    if (millis() - last_roam_check_ms_ < AC_WIFI_ROAM_CHECK_INTERVAL_MS) {
        return;
    }

    last_roam_check_ms_ = millis();
    const int32_t current_rssi = WiFi.RSSI();
    if (current_rssi >= AC_WIFI_ROAM_RSSI_THRESHOLD_DBM) {
        low_rssi_count_ = 0;
        return;
    }

    low_rssi_count_++;
    Log::logf(CAT_WIFI, LOG_DEBUG,
              "[WiFi] low RSSI %ld dBm (%u/%u)\n",
              static_cast<long>(current_rssi),
              static_cast<unsigned>(low_rssi_count_),
              static_cast<unsigned>(AC_WIFI_ROAM_CONSECUTIVE_LOW));
    if (low_rssi_count_ < AC_WIFI_ROAM_CONSECUTIVE_LOW) return;

    WiFi.scanDelete();
    int16_t rc = WiFi.scanNetworks(true);
    if (rc == WIFI_SCAN_FAILED) {
        stats_.roam_scan_failures++;
        low_rssi_count_ = 0;
        return;
    }

    stats_.roam_scans++;
    mode_state_ = WifiModeState::StaRoamScanning;
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] roaming: async scan for better AP\n");
}

int8_t WifiManager::find_profile_by_ssid(const String &ssid) const {
    for (size_t i = 0; i < profile_count_; ++i) {
        if (profiles_[i].ssid == ssid) return static_cast<int8_t>(i);
    }
    return -1;
}

void WifiManager::collect_scan_candidates(int16_t scan_count) {
    scan_candidate_count_ = 0;
    for (int i = 0; i < scan_count &&
                    scan_candidate_count_ < AC_WIFI_SCAN_CANDIDATES_MAX; ++i) {
        const int8_t profile_index = find_profile_by_ssid(WiFi.SSID(i));
        if (profile_index < 0) continue;
        uint8_t *bssid = WiFi.BSSID(i);
        if (!bssid) continue;

        ScanCandidate &candidate = scan_candidates_[scan_candidate_count_++];
        candidate.profile_index = static_cast<uint8_t>(profile_index);
        memcpy(candidate.bssid, bssid, 6);
        candidate.channel = static_cast<uint8_t>(WiFi.channel(i));
        candidate.rssi = static_cast<int8_t>(WiFi.RSSI(i));
    }

    for (size_t i = 0; i < scan_candidate_count_; ++i) {
        for (size_t j = i + 1; j < scan_candidate_count_; ++j) {
            if (scan_candidates_[j].rssi > scan_candidates_[i].rssi) {
                ScanCandidate tmp = scan_candidates_[i];
                scan_candidates_[i] = scan_candidates_[j];
                scan_candidates_[j] = tmp;
            }
        }
    }

    stats_.last_roam_candidates = scan_candidate_count_;
}

bool WifiManager::start_scan_candidate(size_t candidate_index) {
    if (candidate_index >= scan_candidate_count_) return false;
    const ScanCandidate &candidate = scan_candidates_[candidate_index];
    if (candidate.profile_index >= profile_count_) return false;
    WifiProfile &profile = profiles_[candidate.profile_index];
    if (!profile.ssid.length()) return false;

    stop_wifi();
    if (softap_mode_ == SoftApMode::Forced) start_softap(true);
    WiFi.mode(softap_mode_ == SoftApMode::Forced ? WIFI_AP_STA : WIFI_STA);
    apply_country_code();
    WiFi.setHostname(hostname_.c_str());
    mode_state_ = WifiModeState::StaConnecting;
    network_available_ = softap_running_;
    active_profile_index_ = static_cast<int8_t>(candidate.profile_index);
    sta_ssid_ = profile.ssid;
    sta_pass_ = profile.password;
    connect_deadline_ms_ = millis() + AC_WIFI_CONNECT_TIMEOUT_MS;
    pmf_retry_attempted_ = false;
    last_disconnect_reason_ = 0;
    last_disconnect_reason = 0;
    stats_.connect_attempts++;
    roam_connect_pending_ = true;

    char bssid_text[AC_WIFI_BSSID_TEXT_MAX];
    format_bssid(bssid_text, sizeof(bssid_text), candidate.bssid);
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] roaming to profile %u SSID=%s BSSID=%s ch=%u rssi=%d\n",
              static_cast<unsigned>(candidate.profile_index),
              profile.ssid.c_str(), bssid_text,
              static_cast<unsigned>(candidate.channel),
              static_cast<int>(candidate.rssi));
    const char *password = profile.password.length()
                               ? profile.password.c_str()
                               : nullptr;
    WiFi.begin(profile.ssid.c_str(), password, candidate.channel,
               candidate.bssid);
    return true;
}

void WifiManager::handle_roam_scan() {
    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;

    if (result == WIFI_SCAN_FAILED) {
        stats_.roam_scan_failures++;
        low_rssi_count_ = 0;
        if (WiFi.status() == WL_CONNECTED) {
            mode_state_ = WifiModeState::StaConnected;
        } else if (active_profile_index_ >= 0) {
            start_profile(static_cast<size_t>(active_profile_index_));
        } else {
            start_next_profile(0);
        }
        return;
    }

    collect_scan_candidates(result);
    WiFi.scanDelete();

    bool should_switch = false;
    if (scan_candidate_count_ > 0 && WiFi.status() == WL_CONNECTED) {
        uint8_t *current_bssid = WiFi.BSSID();
        const ScanCandidate &best = scan_candidates_[0];
        if (current_bssid && memcmp(best.bssid, current_bssid, 6) != 0) {
            const int32_t current_rssi = WiFi.RSSI();
            const int32_t candidate_rssi = best.rssi;
            if (candidate_rssi > current_rssi + AC_WIFI_ROAM_HYSTERESIS_DB) {
                should_switch = true;
                char bssid_text[AC_WIFI_BSSID_TEXT_MAX];
                format_bssid(bssid_text, sizeof(bssid_text), best.bssid);
                Log::logf(CAT_WIFI, LOG_INFO,
                          "[WiFi] roam candidate BSSID=%s %ld dBm beats "
                          "current %ld dBm by >=%ld\n",
                          bssid_text,
                          static_cast<long>(candidate_rssi),
                          static_cast<long>(current_rssi),
                          static_cast<long>(AC_WIFI_ROAM_HYSTERESIS_DB));
            }
        }
    }

    low_rssi_count_ = 0;
    if (should_switch && start_scan_candidate(0)) return;
    if (WiFi.status() == WL_CONNECTED) {
        mode_state_ = WifiModeState::StaConnected;
    } else if (active_profile_index_ >= 0) {
        start_profile(static_cast<size_t>(active_profile_index_));
    } else {
        start_next_profile(0);
    }
}

void WifiManager::cleanup_manual_scan() {
    if (!manual_scan_active_) return;

    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;

    if (manual_scan_completed_ms_ == 0) {
        manual_scan_completed_ms_ = millis();
        if (result < 0) {
            Log::logf(CAT_WIFI, LOG_WARN,
                      "[WiFi] manual scan failed rc=%d\n",
                      static_cast<int>(result));
        } else {
            Log::logf(CAT_WIFI, LOG_INFO,
                      "[WiFi] manual scan complete count=%d\n",
                      static_cast<int>(result));
        }
        return;
    }

    if (millis() - manual_scan_completed_ms_ <
        AC_WIFI_MANUAL_SCAN_RESULT_TTL_MS) {
        return;
    }

    WiFi.scanDelete();
    manual_scan_active_ = false;
    manual_scan_completed_ms_ = 0;
}

void WifiManager::handle_connect_timeout() {
    last_disconnect_reason_ = last_disconnect_reason;
    stats_.last_disconnect_reason = last_disconnect_reason_;
    if (!pmf_retry_attempted_ && last_disconnect_reason_ == 208) {
        retry_with_pmf_disabled();
        return;
    }

    stats_.connect_failures++;
    roam_connect_pending_ = false;
    consecutive_profile_failures_++;
    Log::logf(CAT_WIFI, LOG_WARN,
              "[WiFi] STA failed for SSID=%s reason=%u\n",
              sta_ssid_.c_str(), static_cast<unsigned>(last_disconnect_reason_));
    WiFi.disconnect(true);
    network_available_ = softap_running_;
    if (profile_count_ > 1 &&
        consecutive_profile_failures_ < profile_count_ &&
        active_profile_index_ >= 0 &&
        start_next_profile(static_cast<size_t>(active_profile_index_ + 1))) {
        return;
    }
    start_softap(false);
}

void WifiManager::retry_with_pmf_disabled() {
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK) {
        config.sta.pmf_cfg.capable = false;
        config.sta.pmf_cfg.required = false;
        esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    pmf_retry_attempted_ = true;
    stats_.pmf_retries++;
    mode_state_ = WifiModeState::StaPmfRetry;
    connect_deadline_ms_ = millis() + AC_WIFI_PMF_RETRY_TIMEOUT_MS;
    last_disconnect_reason_ = 0;
    last_disconnect_reason = 0;
    Log::logf(CAT_WIFI, LOG_INFO,
              "[WiFi] reason 208; retrying with PMF disabled\n");
    esp_wifi_disconnect();
    esp_wifi_connect();
}

WifiScanStatus WifiManager::manual_scan_status() {
    if (mode_state_ == WifiModeState::StaRoamScanning) {
        return WifiScanStatus::RoamInProgress;
    }

    if (manual_scan_active_) {
        const int16_t count = WiFi.scanComplete();
        if (count == WIFI_SCAN_RUNNING) {
            return WifiScanStatus::Running;
        }
        if (count < 0) {
            return WifiScanStatus::Failed;
        }
        return WifiScanStatus::Ready;
    }

    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        return WifiScanStatus::Running;
    }

    return WifiScanStatus::Idle;
}

WifiScanStartResult WifiManager::start_manual_scan() {
    if (mode_state_ == WifiModeState::StaRoamScanning) {
        Log::logf(CAT_WIFI, LOG_DEBUG,
                  "[WiFi] manual scan deferred: roaming scan active\n");
        return WifiScanStartResult::RoamInProgress;
    }

    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        Log::logf(CAT_WIFI, LOG_DEBUG,
                  "[WiFi] manual scan request ignored: scan already running\n");
        return WifiScanStartResult::Running;
    }

    WiFi.scanDelete();
    int16_t rc = WiFi.scanNetworks(true);
    if (rc == WIFI_SCAN_FAILED) {
        Log::logf(CAT_WIFI, LOG_WARN, "[WiFi] manual scan start failed\n");
        return WifiScanStartResult::Failed;
    }

    manual_scan_active_ = true;
    manual_scan_completed_ms_ = 0;
    Log::logf(CAT_WIFI, LOG_INFO, "[WiFi] manual scan started\n");
    return WifiScanStartResult::Started;
}

size_t WifiManager::copy_manual_scan_results(WifiScanNetwork *out,
                                             size_t max) {
    if (!out || !max || !manual_scan_active_) return 0;
    const int16_t count = WiFi.scanComplete();
    if (count < 0 || count == WIFI_SCAN_RUNNING) return 0;

    size_t copied = 0;
    for (int i = 0; i < count && copied < max; ++i) {
        WifiScanNetwork &network = out[copied++];
        network.ssid = WiFi.SSID(i);
        network.rssi = WiFi.RSSI(i);
        network.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    return copied;
}

void WifiManager::clear_manual_scan_results() {
    if (!manual_scan_active_) return;
    WiFi.scanDelete();
    manual_scan_active_ = false;
    manual_scan_completed_ms_ = 0;
}

const char *WifiManager::mode_name() const {
    switch (mode_state_) {
        case WifiModeState::Off: return "off";
        case WifiModeState::StaConnected:
            return softap_running_ ? "sta_ap" : "sta";
        case WifiModeState::StaConnecting:
            return softap_running_ ? "connecting_ap" : "connecting";
        case WifiModeState::StaPmfRetry: return "pmf_retry";
        case WifiModeState::SoftAp: return "softap";
        case WifiModeState::StaRoamScanning: return "roaming";
        case WifiModeState::Failed: return "failed";
        default: return "?";
    }
}

}  // namespace aircannect
