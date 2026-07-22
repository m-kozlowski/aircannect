#pragma once

#include <stdint.h>

#include "as11_therapy_state.h"

namespace aircannect {

struct ActivitySnapshot {
    bool therapy_active = false;
    bool realtime_stream_active = false;
    bool foreground_report_demand = false;
    bool ota_install_active = false;
    bool export_active = false;
    uint32_t generation = 0;
};

struct NetworkSnapshot {
    bool associated = false;
    bool ipv4_ready = false;
    bool management_reachable = false;
    int8_t active_profile = -1;
    uint8_t bssid[6] = {};
    uint32_t generation = 0;
};

struct StorageSnapshot {
    bool mounted = false;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint16_t queue_used = 0;
    uint16_t queue_capacity = 0;
    uint32_t generation = 0;
};

struct DeviceSnapshot {
    bool ready = false;
    As11TherapyState therapy_state = As11TherapyState::Unknown;
    bool clock_valid = false;
    int64_t clock_offset_ms = 0;
    int32_t timezone_offset_minutes = 0;
    uint32_t generation = 0;
};

}  // namespace aircannect
