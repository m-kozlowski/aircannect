#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <stdint.h>

#include "board.h"
#include "softap_mode.h"

namespace aircannect {

enum class WifiModeState {
    Off,
    StaConnected,
    StaConnecting,
    StaApSelecting,
    StaPmfRetry,
    SoftAp,
    StaRoamScanning,
    Failed,
};

struct WifiProfile {
    String ssid;
    String password;
};

struct WifiManagerStats {
    uint32_t connect_attempts = 0;
    uint32_t connect_successes = 0;
    uint32_t connect_failures = 0;
    uint32_t disconnects = 0;
    uint32_t pmf_retries = 0;
    uint32_t roam_scans = 0;
    uint32_t roam_scan_failures = 0;
    uint32_t roam_switches = 0;
    uint32_t last_roam_candidates = 0;
    uint8_t last_disconnect_reason = 0;
};

enum class WifiScanStatus {
    Idle,
    RoamInProgress,
    Running,
    Ready,
    Failed,
};

enum class WifiScanStartResult {
    Started,
    RoamInProgress,
    Running,
    Failed,
};

struct WifiScanNetwork {
    String ssid;
    int32_t rssi = 0;
    bool open = false;
};

class WifiManager {
public:
    bool begin();
    void poll();

    bool network_available() const { return network_available_; }
    bool has_sta_config() const { return sta_configured_; }
    bool sta_is_open() const {
        return sta_configured_ && sta_pass_.length() == 0;
    }
    const String &hostname() const { return hostname_; }
    const String &country_code() const { return country_code_; }
    const String &sta_ssid() const { return sta_ssid_; }
    WifiModeState mode_state() const { return mode_state_; }
    const char *state_name() const;
    IPAddress ip() const;
    IPAddress gateway() const;
    IPAddress softap_ip() const;
    int32_t rssi() const;
    int32_t channel() const;
    uint32_t connect_timeout_remaining_ms() const;
    void bssid(char *out, size_t size) const;
    size_t profile_count() const { return profile_count_; }
    const WifiProfile &profile(size_t index) const { return profiles_[index]; }
    int8_t active_profile_index() const { return active_profile_index_; }
    const WifiManagerStats &stats() const { return stats_; }

    void set_hostname(const String &hostname);
    void set_softap_mode(SoftApMode mode);
    SoftApMode softap_mode() const { return softap_mode_; }
    bool softap_running() const { return softap_running_; }
    void apply_softap_mode();
    void set_country_code(const String &country);
    void set_roaming_suspended(bool suspended);
    bool roaming_enabled() const {
        return sta_configured_ && profile_count_ > 0;
    }
    bool roaming_suspended() const { return roaming_suspended_; }
    bool configure_sta(const String &ssid, const String &password);
    bool configure_open_sta(const String &ssid);
    bool add_profile(const String &ssid, const String &password,
                     bool open_network);
    bool replace_profiles(const WifiProfile *profiles, size_t count,
                          bool reconnect_now);
    bool remove_profile(size_t index);
    void clear_sta_config();
    bool reconnect();
    WifiScanStartResult start_manual_scan();
    WifiScanStatus manual_scan_status();
    size_t copy_manual_scan_results(WifiScanNetwork *out, size_t max);
    void clear_manual_scan_results();

private:
    void load_config();
    void save_config(size_t first_dirty_index = 0);

    bool start_profile(size_t index, bool keep_softap = false);
    bool start_next_profile(size_t start_index, bool keep_softap = false);
    bool start_softap(bool with_sta);
    void stop_wifi();
    void apply_country_code();
    void handle_connected();
    void handle_ap_select_scan();
    void handle_connect_timeout();
    void enter_softap_fallback();
    void maybe_retry_softap_sta();
    void maybe_start_roam_scan();
    void handle_roam_scan();
    void cleanup_manual_scan();
    void apply_sta_phy_config();
    bool begin_unpinned_profile(size_t index, bool keep_softap,
                                const char *reason);
    bool begin_scan_candidate(size_t candidate_index, bool keep_softap,
                              bool roaming);

    int8_t find_profile_by_ssid(const String &ssid) const;
    void collect_scan_candidates(int16_t scan_count,
                                 int8_t profile_filter = -1);
    bool start_scan_candidate(size_t candidate_index);
    void retry_with_pmf_disabled();
    const char *mode_name() const;

    struct ScanCandidate {
        uint8_t profile_index = 0;
        uint8_t bssid[6] = {};
        uint8_t channel = 0;
        int8_t rssi = -127;
    };

    bool network_available_ = false;
    bool sta_configured_ = false;
    SoftApMode softap_mode_ = SoftApMode::Auto;
    bool softap_running_ = false;
    bool softap_auto_close_deferred_ = false;
    bool roaming_suspended_ = false;
    bool roam_connect_pending_ = false;
    bool ap_select_keep_softap_ = false;
    bool manual_scan_active_ = false;
    bool pmf_retry_attempted_ = false;
    uint8_t last_disconnect_reason_ = 0;
    uint32_t connect_deadline_ms_ = 0;
    uint32_t ap_select_deadline_ms_ = 0;
    uint32_t softap_retry_deadline_ms_ = 0;
    uint32_t last_roam_check_ms_ = 0;
    uint32_t manual_scan_completed_ms_ = 0;

    String hostname_ = AC_HOSTNAME;
    String country_code_;
    String sta_ssid_;
    String sta_pass_;

    WifiProfile profiles_[AC_WIFI_PROFILE_MAX];
    size_t profile_count_ = 0;
    int8_t active_profile_index_ = -1;
    uint8_t consecutive_profile_failures_ = 0;
    uint8_t low_rssi_count_ = 0;
    ScanCandidate scan_candidates_[AC_WIFI_SCAN_CANDIDATES_MAX];
    size_t scan_candidate_count_ = 0;

    WifiManagerStats stats_;
    WifiModeState mode_state_ = WifiModeState::Off;
};

}  // namespace aircannect
