#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"

namespace aircannect {

static constexpr uint32_t AS11_BLE_FIG_SYNC = 0xCAFEBABEu;
static constexpr size_t AS11_BLE_FIG_PREFIX_BYTES = 16;
static constexpr size_t AS11_BLE_FIG_MAX_PAYLOAD_BYTES = 7682;
static constexpr size_t AS11_BLE_FIG_MAX_PACKET_BYTES =
    AS11_BLE_FIG_PREFIX_BYTES + AS11_BLE_FIG_MAX_PAYLOAD_BYTES;

static constexpr uint16_t AS11_BLE_VCID_PLAINTEXT_REQUEST = 0x0393;
static constexpr uint16_t AS11_BLE_VCID_PLAINTEXT_RESPONSE = 0x0392;
static constexpr uint16_t AS11_BLE_VCID_ENCRYPTED_REQUEST = 0x0397;
static constexpr uint16_t AS11_BLE_VCID_ENCRYPTED_RESPONSE = 0x0396;

enum class As11BleFigDecodeState : uint8_t {
    NeedMore,
    Packet,
    HeaderCrcError,
    PayloadCrcError,
    PayloadTooLarge,
    BufferUnavailable,
};

struct As11BleFigPacket {
    uint16_t vcid = 0;
    std::shared_ptr<const LargeByteBuffer> payload;
};

class As11BleFigCodec {
public:
    static std::unique_ptr<LargeByteBuffer> encode(uint16_t vcid,
                                                   const uint8_t *payload,
                                                   size_t payload_length);

    bool begin();
    bool feed(const uint8_t *data, size_t length);
    As11BleFigDecodeState take(As11BleFigPacket &packet);
    void reset();

private:
    bool compact_for(size_t incoming);
    size_t find_sync() const;

    std::unique_ptr<LargeByteBuffer> buffer_;
    size_t begin_ = 0;
    size_t end_ = 0;
};

}  // namespace aircannect
