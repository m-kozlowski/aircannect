#include "oximetry_hub.h"

#include <string.h>

#include "board_oximetry.h"

namespace aircannect {

void OximetryHub::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) clear_source();
}

bool OximetryHub::ingest(const OximetrySample &sample,
                         uint32_t now_ms,
                         OximetryHubAction &actions) {
    actions = OximetryHubAction::None;
    if (!enabled_ || sample.source == OximetrySource::None) return false;
    if (source_present_ && source_ != sample.source) return false;
    if (!source_present_ && !sample.valid &&
        sample.source != OximetrySource::Ble) {
        return false;
    }

    source_present_ = true;
    source_ = sample.source;
    last_source_ms_ = now_ms;
    strncpy(source_detail_, sample.detail, sizeof(source_detail_) - 1);
    source_detail_[sizeof(source_detail_) - 1] = 0;

    reading_.spo2 = sample.valid ? sample.spo2 : -1;
    reading_.pulse_bpm = sample.valid ? sample.pulse_bpm : -1;
    reading_.valid = sample.valid;
    reading_.contact_known = sample.contact_known;
    reading_.contact_present = sample.contact_present;
    reading_.timestamp_ms = now_ms;

    if (sample.source != OximetrySource::Ble || sample.valid) {
        ble_invalid_since_ms_ = 0;
        return true;
    }

    if (!ble_invalid_since_ms_) ble_invalid_since_ms_ = now_ms;
    if (static_cast<int32_t>(now_ms - ble_invalid_since_ms_) >=
        static_cast<int32_t>(AC_OXIMETRY_SENSOR_INVALID_DISCONNECT_MS)) {
        actions = OximetryHubAction::DisconnectBleSensor;
        ble_invalid_since_ms_ = now_ms;
    }
    return true;
}

OximetryHubAction OximetryHub::poll(uint32_t now_ms) {
    if (!enabled_ || !source_present_ || source_alive(now_ms)) {
        return OximetryHubAction::None;
    }

    const bool disconnect_ble = source_ == OximetrySource::Ble;
    clear_source();
    return OximetryHubAction::SourceBecameStale |
           (disconnect_ble ? OximetryHubAction::DisconnectBleSensor
                           : OximetryHubAction::None);
}

void OximetryHub::source_disconnected(OximetrySource source) {
    if (source_present_ && source_ == source) clear_source();
}

OximetryHubSnapshot OximetryHub::snapshot(uint32_t now_ms) const {
    OximetryHubSnapshot out;
    out.enabled = enabled_;
    out.source_present = source_present_;
    out.source_fresh = sample_fresh(now_ms);
    out.source = source_;
    out.reading = reading_;
    out.last_source_age_ms = source_present_ ? now_ms - last_source_ms_ : 0;
    strncpy(out.source_detail, source_detail_, sizeof(out.source_detail) - 1);
    out.source_detail[sizeof(out.source_detail) - 1] = 0;
    return out;
}

void OximetryHub::clear_source() {
    source_present_ = false;
    source_ = OximetrySource::None;
    reading_ = OximetryReading{};
    source_detail_[0] = 0;
    last_source_ms_ = 0;
    ble_invalid_since_ms_ = 0;
}

bool OximetryHub::source_alive(uint32_t now_ms) const {
    const uint32_t timeout =
        source_ == OximetrySource::Ble
            ? AC_OXIMETRY_SENSOR_NOTIFY_TIMEOUT_MS
            : AC_OXIMETRY_SOURCE_TIMEOUT_MS;
    return static_cast<int32_t>(now_ms - last_source_ms_) <
           static_cast<int32_t>(timeout);
}

bool OximetryHub::sample_fresh(uint32_t now_ms) const {
    return source_present_ &&
           static_cast<int32_t>(now_ms - last_source_ms_) <
               static_cast<int32_t>(AC_OXIMETRY_SAMPLE_STALE_MS);
}

}  // namespace aircannect
