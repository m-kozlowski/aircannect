#pragma once

#include <array>
#include <stdint.h>
#include <string>
#include <vector>

namespace aircannect {

struct DatagramFrame {
    uint8_t len = 0;
    std::array<uint8_t, 8> data = {};
};

enum class DatagramStatus {
    InProgress,
    Complete,
    Error,
};

struct DatagramFeedResult {
    DatagramStatus status = DatagramStatus::InProgress;
    std::string payload;
    std::string error;
};

uint32_t crc32_ieee(const uint8_t *data, size_t len);
std::vector<DatagramFrame> encode_datagram(const uint8_t *payload, size_t len);
std::vector<DatagramFrame> encode_datagram(const std::string &payload);
std::string hex_bytes(const uint8_t *data, size_t len);

class DatagramRx {
public:
    DatagramFeedResult feed(const uint8_t *data,
                            size_t len,
                            uint32_t now_ms = 0);
    DatagramFeedResult poll(uint32_t now_ms);
    void reset();

private:
    bool append_bytes(const uint8_t *data,
                      size_t len,
                      DatagramFeedResult &result);
    void note_frame(uint32_t now_ms);

    std::vector<uint8_t> parts_;
    uint32_t expected_crc_ = 0;
    uint32_t last_frame_ms_ = 0;
    bool have_crc_ = false;
};

}  // namespace aircannect
