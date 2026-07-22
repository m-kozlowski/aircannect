#pragma once

#include <WiFiUdp.h>
#include <stdint.h>

#include "oximetry_hub.h"
#include "oximetry_status.h"

namespace aircannect {

class UdpOximeterSource {
public:
    void configure(bool enabled, uint16_t port);
    OximetryHubAction poll(bool network_available,
                           uint32_t now_ms,
                           OximetryHub &hub);
    void stop();

    UdpOximeterStatus status() const { return status_; }

private:
    enum class ReadResult : uint8_t {
        Empty,
        Rejected,
        Sample,
    };

    bool ensure_started(bool network_available);
    ReadResult read_sample(OximetrySample &sample);
    void set_error(const char *text);

    WiFiUDP udp_;
    UdpOximeterStatus status_;
};

}  // namespace aircannect
