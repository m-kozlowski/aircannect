#pragma once

#include <array>
#include <stddef.h>
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
    const char *payload_data = nullptr;
    size_t payload_len = 0;
    std::string error;

    bool has_payload() const { return payload_data != nullptr; }
    std::string payload_string() const {
        return payload_data ? std::string(payload_data, payload_len)
                            : std::string();
    }
};

uint32_t crc32_ieee(const uint8_t *data, size_t len);
std::vector<DatagramFrame> encode_datagram(const uint8_t *payload, size_t len);
std::vector<DatagramFrame> encode_datagram(const std::string &payload);
std::string hex_bytes(const uint8_t *data, size_t len);

class DatagramRx {
public:
    DatagramRx();
    explicit DatagramRx(size_t initial_reserve);
    ~DatagramRx();

    DatagramRx(const DatagramRx &) = delete;
    DatagramRx &operator=(const DatagramRx &) = delete;

    DatagramFeedResult feed(const uint8_t *data,
                            size_t len,
                            uint32_t now_ms = 0);
    DatagramFeedResult poll(uint32_t now_ms);
    void reset();

private:
    bool append_bytes(const uint8_t *data,
                      size_t len,
                      DatagramFeedResult &result);
    bool reserve_parts(size_t capacity, DatagramFeedResult &result);
    void set_payload_view(DatagramFeedResult &result) const;
    void note_frame(uint32_t now_ms);

    uint8_t *parts_ = nullptr;
    size_t parts_len_ = 0;
    size_t parts_capacity_ = 0;
    size_t initial_reserve_ = 0;
    uint32_t expected_crc_ = 0;
    uint32_t last_frame_ms_ = 0;
    bool have_crc_ = false;
};

}  // namespace aircannect
