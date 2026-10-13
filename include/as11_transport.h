#pragma once

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "board_ble.h"
#include "board_can.h"

namespace aircannect {

#ifndef AC_DEFAULT_AS11_TRANSPORT_BLE
#define AC_DEFAULT_AS11_TRANSPORT_BLE (!AC_CAN_ENABLED)
#endif

enum class As11Transport : uint8_t {
    Can = 0,
    Ble = 1,
};

static_assert(AC_CAN_ENABLED || AC_BLE_ENABLED,
              "AirCANnect requires at least one AS11 transport");
static_assert(AC_DEFAULT_AS11_TRANSPORT_BLE ? AC_BLE_ENABLED : AC_CAN_ENABLED,
              "The default AS11 transport must be enabled");

inline const char *as11_transport_name(As11Transport transport) {
    return transport == As11Transport::Ble ? "ble" : "can";
}

inline bool as11_transport_valid(As11Transport transport) {
    return transport == As11Transport::Can || transport == As11Transport::Ble;
}

inline bool as11_transport_supported(As11Transport transport) {
    if (!as11_transport_valid(transport)) return false;
    if (transport == As11Transport::Can) return AC_CAN_ENABLED != 0;
    return AC_BLE_ENABLED != 0;
}

inline constexpr As11Transport default_as11_transport() {
#if AC_DEFAULT_AS11_TRANSPORT_BLE
    return As11Transport::Ble;
#else
    return As11Transport::Can;
#endif
}

inline bool parse_as11_transport(const char *value,
                                 As11Transport &transport) {
    if (!value) return false;
    if (strcasecmp(value, "can") == 0 || strcmp(value, "0") == 0) {
        transport = As11Transport::Can;
        return true;
    }
    if (strcasecmp(value, "ble") == 0 ||
        strcasecmp(value, "bluetooth") == 0 || strcmp(value, "1") == 0) {
        transport = As11Transport::Ble;
        return true;
    }
    return false;
}

}  // namespace aircannect
