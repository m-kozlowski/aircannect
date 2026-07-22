#include "udp_oximeter_source.h"

#include <stdio.h>
#include <string.h>

#include "board_oximetry.h"
#include "debug_log.h"
#include "oximetry_codec.h"

namespace aircannect {

void UdpOximeterSource::configure(bool enabled, uint16_t port) {
    const bool port_changed = status_.port != port;
    status_.enabled = enabled;
    status_.port = port;

    if (!enabled || port_changed) stop();
}

OximetryHubAction UdpOximeterSource::poll(bool network_available,
                                          uint32_t now_ms,
                                          OximetryHub &hub) {
    if (!ensure_started(network_available)) return OximetryHubAction::None;

    OximetryHubAction accumulated = OximetryHubAction::None;
    for (size_t i = 0; i < AC_OXIMETRY_UDP_READ_BUDGET; ++i) {
        OximetrySample sample;
        const ReadResult result = read_sample(sample);
        if (result == ReadResult::Empty) break;
        if (result == ReadResult::Rejected) continue;

        OximetryHubAction actions = OximetryHubAction::None;
        if (hub.ingest(sample, now_ms, actions)) status_.packets++;
        accumulated = accumulated | actions;
    }
    return accumulated;
}

void UdpOximeterSource::stop() {
    if (!status_.started) return;
    udp_.stop();
    status_.started = false;
}

bool UdpOximeterSource::ensure_started(bool network_available) {
    if (!status_.enabled || !network_available) {
        stop();
        return false;
    }
    if (status_.started) return true;
    if (!status_.port || !udp_.begin(status_.port)) {
        set_error("UDP bind failed");
        return false;
    }

    status_.started = true;
    set_error("");
    Log::logf(CAT_OXI, LOG_INFO, "UDP listening on port %u\n",
              status_.port);
    return true;
}

UdpOximeterSource::ReadResult UdpOximeterSource::read_sample(
    OximetrySample &sample) {
    const int packet_size = udp_.parsePacket();
    if (packet_size <= 0) return ReadResult::Empty;

    const IPAddress remote_ip = udp_.remoteIP();
    uint8_t packet[AC_OXIMETRY_UDP_PACKET_SIZE] = {};
    const int read = udp_.read(packet, sizeof(packet));
    while (udp_.available()) udp_.read();

    if (packet_size != static_cast<int>(AC_OXIMETRY_UDP_PACKET_SIZE) ||
        read != static_cast<int>(AC_OXIMETRY_UDP_PACKET_SIZE) ||
        packet[0] != 0x55 || packet[1] != 0xab) {
        status_.bad_packets++;
        return ReadResult::Rejected;
    }

    const uint16_t spo2_raw =
        static_cast<uint16_t>(packet[3]) |
        (static_cast<uint16_t>(packet[4]) << 8);
    const uint16_t pulse_raw =
        static_cast<uint16_t>(packet[5]) |
        (static_cast<uint16_t>(packet[6]) << 8);
    bool spo2_valid = false;
    bool pulse_valid = false;

    sample.source = OximetrySource::Udp;
    sample.spo2 = decode_sfloat_int_value(spo2_raw, spo2_valid);
    sample.pulse_bpm = decode_sfloat_int_value(pulse_raw, pulse_valid);
    sample.valid = spo2_valid && pulse_valid;
    snprintf(sample.detail, sizeof(sample.detail), "%u.%u.%u.%u",
             remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3]);
    return ReadResult::Sample;
}

void UdpOximeterSource::set_error(const char *text) {
    strncpy(status_.last_error, text ? text : "",
            sizeof(status_.last_error) - 1);
    status_.last_error[sizeof(status_.last_error) - 1] = 0;
}

}  // namespace aircannect
