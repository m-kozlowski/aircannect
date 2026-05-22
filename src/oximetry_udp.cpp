#include "oximetry_internal.h"

namespace aircannect {

bool OximetryManager::ensure_udp(bool network_available) {
    if (!status_.enabled || !network_available) {
        stop_udp();
        return false;
    }
    if (status_.udp_started) return true;
    if (!udp_.begin(status_.udp_port)) {
        set_error("UDP bind failed");
        return false;
    }
    status_.udp_started = true;
    Log::logf(CAT_OXI, LOG_INFO, "[OXI] UDP listening on port %u\n",
              status_.udp_port);
    return true;
}

void OximetryManager::stop_udp() {
    if (!status_.udp_started) return;
    udp_.stop();
    status_.udp_started = false;
}

void OximetryManager::poll_udp(uint32_t now_ms) {
    if (!status_.udp_started) return;
    for (size_t i = 0; i < AC_OXIMETRY_UDP_READ_BUDGET; ++i) {
        const int packet_size = udp_.parsePacket();
        if (packet_size <= 0) return;
        const IPAddress remote_ip = udp_.remoteIP();
        uint8_t packet[AC_OXIMETRY_UDP_PACKET_SIZE] = {};
        const int read = udp_.read(packet, sizeof(packet));
        while (udp_.available()) udp_.read();

        if (packet_size != static_cast<int>(AC_OXIMETRY_UDP_PACKET_SIZE) ||
            read != static_cast<int>(AC_OXIMETRY_UDP_PACKET_SIZE) ||
            packet[0] != 0x55 || packet[1] != 0xab) {
            status_.udp_bad_packets++;
            continue;
        }

        const uint16_t spo2_raw =
            static_cast<uint16_t>(packet[3]) |
            (static_cast<uint16_t>(packet[4]) << 8);
        const uint16_t pulse_raw =
            static_cast<uint16_t>(packet[5]) |
            (static_cast<uint16_t>(packet[6]) << 8);
        note_udp_packet(spo2_raw, pulse_raw, remote_ip, now_ms);
    }
}

void OximetryManager::note_udp_packet(uint16_t spo2_raw,
                                      uint16_t pulse_raw,
                                      IPAddress remote_ip,
                                      uint32_t now_ms) {
    char source_detail[48];
    snprintf(source_detail, sizeof(source_detail),
             "%u.%u.%u.%u",
             remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3]);
    (void)note_source_packet(OximetrySource::Udp, source_detail, spo2_raw,
                             pulse_raw, false, false, false, now_ms);
}

}  // namespace aircannect
