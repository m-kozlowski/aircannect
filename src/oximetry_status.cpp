#include "oximetry_status.h"

#include <string.h>

namespace aircannect {

OximetryRuntimeStatus compose_oximetry_status(
    const OximetryHubSnapshot &hub,
    const UdpOximeterStatus &udp,
    const PlxPeripheralStatus &peripheral) {
    OximetryRuntimeStatus out;
    out.enabled = hub.enabled;
    out.advertise_mode = peripheral.advertise_mode;
    out.source_present = hub.source_present;
    out.source_fresh = hub.source_fresh;
    out.source = hub.source;
    out.reading = hub.reading;
    out.last_source_age_ms = hub.last_source_age_ms;
    strncpy(out.source_detail, hub.source_detail,
            sizeof(out.source_detail) - 1);

    out.udp_port = udp.port;
    out.udp_started = udp.started;
    out.udp_packets = udp.packets;
    out.udp_bad_packets = udp.bad_packets;

    out.ble_available = peripheral.ble_available;
    out.advertising = peripheral.advertising;
    out.connected = peripheral.connected;
    out.subscribed = peripheral.subscribed;
    out.manual_advertising_requested =
        peripheral.manual_advertising_requested;
    out.pairing_active = peripheral.pairing_active;
    out.pairing_left_ms = peripheral.pairing_left_ms;
    out.ble_connections = peripheral.connections;
    out.ble_disconnects = peripheral.disconnects;
    out.ble_last_disconnect_reason = peripheral.last_disconnect_reason;
    out.ble_notifications = peripheral.notifications;
    out.ble_invalid_notifications = peripheral.invalid_notifications;
    strncpy(out.ble_name, peripheral.name, sizeof(out.ble_name) - 1);
    strncpy(out.ble_peer, peripheral.peer, sizeof(out.ble_peer) - 1);

    const char *error = peripheral.last_error[0]
                            ? peripheral.last_error
                            : udp.last_error;
    strncpy(out.last_error, error, sizeof(out.last_error) - 1);
    return out;
}

}  // namespace aircannect
