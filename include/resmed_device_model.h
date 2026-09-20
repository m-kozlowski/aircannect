#pragma once

#include <stdint.h>

namespace aircannect {

enum class ResmedDeviceModel : uint8_t {
    Unknown,
    AirSense11,
    AirMini,
};

inline ResmedDeviceModel resmed_device_model(int32_t platform_id) {
    switch (platform_id) {
        case 46: return ResmedDeviceModel::AirSense11;
        case 39: return ResmedDeviceModel::AirMini;
        default: return ResmedDeviceModel::Unknown;
    }
}

}  // namespace aircannect
