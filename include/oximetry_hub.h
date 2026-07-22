#pragma once

#include <stdint.h>

namespace aircannect {

enum class OximetrySource : uint8_t {
    None,
    Udp,
    Ble,
};

struct OximetryReading {
    int16_t spo2 = -1;
    int16_t pulse_bpm = -1;
    bool valid = false;
    bool contact_known = false;
    bool contact_present = false;
    uint32_t timestamp_ms = 0;
};

struct OximetrySample {
    OximetrySource source = OximetrySource::None;
    int16_t spo2 = -1;
    int16_t pulse_bpm = -1;
    bool valid = false;
    bool contact_known = false;
    bool contact_present = false;
    char detail[48] = {};
};

enum class OximetryHubAction : uint8_t {
    None = 0,
    DisconnectBleSensor = 1 << 0,
    SourceBecameStale = 1 << 1,
};

constexpr OximetryHubAction operator|(OximetryHubAction lhs,
                                      OximetryHubAction rhs) {
    return static_cast<OximetryHubAction>(
        static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

constexpr bool oximetry_action_has(OximetryHubAction actions,
                                   OximetryHubAction action) {
    return (static_cast<uint8_t>(actions) &
            static_cast<uint8_t>(action)) != 0;
}

struct OximetryHubSnapshot {
    bool enabled = false;
    bool source_present = false;
    bool source_fresh = false;
    OximetrySource source = OximetrySource::None;
    char source_detail[48] = {};
    OximetryReading reading;
    uint32_t last_source_age_ms = 0;
};

class OximetryHub {
public:
    void set_enabled(bool enabled);
    bool ingest(const OximetrySample &sample,
                uint32_t now_ms,
                OximetryHubAction &actions);
    OximetryHubAction poll(uint32_t now_ms);
    void source_disconnected(OximetrySource source);

    OximetryHubSnapshot snapshot(uint32_t now_ms) const;

private:
    void clear_source();
    bool source_alive(uint32_t now_ms) const;
    bool sample_fresh(uint32_t now_ms) const;

    bool enabled_ = false;
    bool source_present_ = false;
    OximetrySource source_ = OximetrySource::None;
    OximetryReading reading_;
    char source_detail_[48] = {};
    uint32_t last_source_ms_ = 0;
    uint32_t ble_invalid_since_ms_ = 0;
};

}  // namespace aircannect
