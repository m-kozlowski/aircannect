#include "as11_ble_fig.h"

#include <string.h>

#include "crc32.h"

namespace aircannect {

namespace {

uint16_t get_le16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

uint32_t get_le32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

void put_le16(uint8_t *data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}

void put_le32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

std::unique_ptr<LargeByteBuffer> As11BleFigCodec::encode(
    uint16_t vcid,
    const uint8_t *payload,
    size_t payload_length) {
    if ((!payload && payload_length != 0) ||
        payload_length > AS11_BLE_FIG_MAX_PAYLOAD_BYTES) {
        return {};
    }

    const size_t packet_length = AS11_BLE_FIG_PREFIX_BYTES + payload_length;
    std::unique_ptr<LargeByteBuffer> packet =
        LargeByteBuffer::allocate(packet_length);
    if (!packet) return {};

    uint8_t *out = packet->data();
    put_le32(out, AS11_BLE_FIG_SYNC);
    put_le16(out + 4, vcid);
    put_le16(out + 6, static_cast<uint16_t>(payload_length));
    put_le32(out + 8, crc32_ieee(payload, payload_length));
    put_le32(out + 12, crc32_ieee(out + 4, 8));
    if (payload_length != 0) {
        memcpy(out + AS11_BLE_FIG_PREFIX_BYTES, payload, payload_length);
    }
    return packet;
}

bool As11BleFigCodec::begin() {
    if (buffer_) return true;
    buffer_ = LargeByteBuffer::allocate(AS11_BLE_FIG_MAX_PACKET_BYTES + 3);
    return buffer_ != nullptr;
}

bool As11BleFigCodec::feed(const uint8_t *data, size_t length) {
    if ((!data && length != 0) || length > AS11_BLE_FIG_MAX_PACKET_BYTES) {
        return false;
    }
    if (!begin() || !compact_for(length)) return false;

    if (length != 0) memcpy(buffer_->data() + end_, data, length);
    end_ += length;
    return true;
}

As11BleFigDecodeState As11BleFigCodec::take(As11BleFigPacket &packet) {
    packet = {};
    if (!buffer_) return As11BleFigDecodeState::BufferUnavailable;

    const size_t sync = find_sync();
    if (sync == end_) {
        const size_t retained = end_ - begin_ > 3 ? 3 : end_ - begin_;
        if (retained != 0) {
            memmove(buffer_->data(), buffer_->data() + end_ - retained,
                    retained);
        }
        begin_ = 0;
        end_ = retained;
        return As11BleFigDecodeState::NeedMore;
    }
    begin_ = sync;

    if (end_ - begin_ < AS11_BLE_FIG_PREFIX_BYTES) {
        return As11BleFigDecodeState::NeedMore;
    }

    const uint8_t *header = buffer_->data() + begin_ + 4;
    if (crc32_ieee(header, 8) != get_le32(header + 8)) {
        begin_ += 4;
        return As11BleFigDecodeState::HeaderCrcError;
    }

    const size_t payload_length = get_le16(header + 2);
    if (payload_length > AS11_BLE_FIG_MAX_PAYLOAD_BYTES) {
        begin_ += 4;
        return As11BleFigDecodeState::PayloadTooLarge;
    }

    const size_t packet_length = AS11_BLE_FIG_PREFIX_BYTES + payload_length;
    if (end_ - begin_ < packet_length) {
        return As11BleFigDecodeState::NeedMore;
    }

    const uint8_t *payload = buffer_->data() + begin_ +
                             AS11_BLE_FIG_PREFIX_BYTES;
    if (crc32_ieee(payload, payload_length) != get_le32(header + 4)) {
        begin_ += 4;
        return As11BleFigDecodeState::PayloadCrcError;
    }

    packet.vcid = get_le16(header);
    if (payload_length != 0) {
        packet.payload = LargeByteBuffer::copy_and_freeze(payload,
                                                          payload_length);
        if (!packet.payload) {
            return As11BleFigDecodeState::BufferUnavailable;
        }
    }
    begin_ += packet_length;
    if (begin_ == end_) begin_ = end_ = 0;
    return As11BleFigDecodeState::Packet;
}

void As11BleFigCodec::reset() {
    begin_ = 0;
    end_ = 0;
}

bool As11BleFigCodec::compact_for(size_t incoming) {
    if (incoming <= buffer_->size() - end_) return true;

    const size_t retained = end_ - begin_;
    if (incoming > buffer_->size() - retained) return false;
    if (retained != 0) {
        memmove(buffer_->data(), buffer_->data() + begin_, retained);
    }
    begin_ = 0;
    end_ = retained;
    return true;
}

size_t As11BleFigCodec::find_sync() const {
    static constexpr uint8_t SYNC[] = {0xBE, 0xBA, 0xFE, 0xCA};
    if (end_ - begin_ < sizeof(SYNC)) return end_;

    const uint8_t *data = buffer_->data();
    for (size_t offset = begin_; offset <= end_ - sizeof(SYNC); ++offset) {
        if (memcmp(data + offset, SYNC, sizeof(SYNC)) == 0) return offset;
    }
    return end_;
}

}  // namespace aircannect
