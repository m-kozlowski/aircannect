#include "as11_service_protocol.h"

#include <algorithm>
#include <limits.h>
#include <string.h>

#include "crc32.h"
#include "runtime_clock.h"

namespace aircannect {
namespace {

static constexpr uint8_t ISO_TP_PCI_SHIFT = 4;
static constexpr uint8_t ISO_TP_PCI_VALUE_MASK = 0x0F;
static constexpr size_t ISO_TP_SINGLE_DATA_BYTES = 7;
static constexpr size_t ISO_TP_FIRST_DATA_BYTES = 6;
static constexpr size_t ISO_TP_CONSECUTIVE_DATA_BYTES = 7;
static constexpr size_t ISO_TP_MAX_CLASSIC_PACKET_BYTES = 0x0FFF;

static_assert(AS11_SERVICE_PACKET_MAX_BYTES ==
                  ISO_TP_MAX_CLASSIC_PACKET_BYTES,
              "service packet limit must match classic-CAN ISO-TP");

uint16_t get_le16(const uint8_t *value) {
    return static_cast<uint16_t>(value[0]) |
           (static_cast<uint16_t>(value[1]) << 8);
}

uint32_t get_le32(const uint8_t *value) {
    return static_cast<uint32_t>(value[0]) |
           (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) |
           (static_cast<uint32_t>(value[3]) << 24);
}

}  // namespace

const char *as11_service_packet_error_name(As11ServicePacketError error) {
    switch (error) {
        case As11ServicePacketError::None: return "none";
        case As11ServicePacketError::TooShort: return "packet_too_short";
        case As11ServicePacketError::TooLarge: return "packet_too_large";
        case As11ServicePacketError::BadMagic: return "bad_magic";
        case As11ServicePacketError::UnsupportedVersion:
            return "unsupported_version";
        case As11ServicePacketError::LengthMismatch:
            return "length_mismatch";
        case As11ServicePacketError::CrcMismatch: return "crc_mismatch";
    }
    return "unknown";
}

As11ServicePacketError as11_service_validate_packet(
    const uint8_t *packet,
    size_t packet_size,
    As11ServicePacketHeader &header) {
    header = {};
    if (!packet || packet_size < AS11_SERVICE_PACKET_OVERHEAD) {
        return As11ServicePacketError::TooShort;
    }
    if (packet_size > AS11_SERVICE_PACKET_MAX_BYTES) {
        return As11ServicePacketError::TooLarge;
    }
    if (packet[0] != AS11_SERVICE_PACKET_MAGIC) {
        return As11ServicePacketError::BadMagic;
    }
    if (packet[1] != AS11_SERVICE_PROTOCOL_VERSION) {
        return As11ServicePacketError::UnsupportedVersion;
    }

    header.command = packet[2];
    header.status = packet[3];
    header.sequence = get_le16(packet + 4);
    header.payload_length = get_le16(packet + 6);

    const size_t expected_size =
        AS11_SERVICE_PACKET_OVERHEAD + header.payload_length;
    if (packet_size != expected_size) {
        return As11ServicePacketError::LengthMismatch;
    }

    const size_t crc_offset =
        AS11_SERVICE_PACKET_HEADER_BYTES + header.payload_length;
    const uint32_t expected_crc = get_le32(packet + crc_offset);
    const uint32_t actual_crc = crc32_ieee(packet, crc_offset);
    if (actual_crc != expected_crc) {
        return As11ServicePacketError::CrcMismatch;
    }
    return As11ServicePacketError::None;
}

bool as11_isotp_frame_type(const uint8_t *frame,
                           size_t frame_size,
                           As11IsoTpFrameType &type) {
    type = As11IsoTpFrameType::Single;
    if (!frame || frame_size == 0 || frame_size > 8) return false;

    const uint8_t raw_type = frame[0] >> ISO_TP_PCI_SHIFT;
    if (raw_type > static_cast<uint8_t>(As11IsoTpFrameType::FlowControl)) {
        return false;
    }

    type = static_cast<As11IsoTpFrameType>(raw_type);
    return true;
}

bool as11_isotp_parse_flow_control(
    const uint8_t *frame,
    size_t frame_size,
    As11IsoTpFlowControl &flow_control) {
    flow_control = {};

    As11IsoTpFrameType type;
    if (!as11_isotp_frame_type(frame, frame_size, type) ||
        type != As11IsoTpFrameType::FlowControl || frame_size < 3) {
        return false;
    }

    const uint8_t raw_status = frame[0] & ISO_TP_PCI_VALUE_MASK;
    if (raw_status > static_cast<uint8_t>(As11IsoTpFlowStatus::Overflow)) {
        return false;
    }

    flow_control.status = static_cast<As11IsoTpFlowStatus>(raw_status);
    flow_control.block_size = frame[1];
    flow_control.st_min = frame[2];
    return true;
}

bool as11_isotp_make_flow_control(uint8_t block_size,
                                  uint8_t st_min,
                                  As11IsoTpCanFrame &frame) {
    uint32_t delay_ms = 0;
    if (!as11_isotp_st_min_delay_ms(st_min, delay_ms)) return false;

    frame = {};
    frame.len = 3;
    frame.data[0] =
        static_cast<uint8_t>(As11IsoTpFrameType::FlowControl)
        << ISO_TP_PCI_SHIFT;
    frame.data[1] = block_size;
    frame.data[2] = st_min;
    return true;
}

bool as11_isotp_st_min_delay_ms(uint8_t st_min, uint32_t &delay_ms) {
    delay_ms = 0;
    if (st_min <= 0x7F) {
        delay_ms = st_min;
        return true;
    }

    if (st_min >= 0xF1 && st_min <= 0xF9) {
        // The manager is millisecond-driven. Rounding a 100-900 us value up
        // preserves the peer's minimum separation requirement.
        delay_ms = 1;
        return true;
    }
    return false;
}

size_t as11_isotp_frame_count(size_t packet_size) {
    if (packet_size == 0 ||
        packet_size > ISO_TP_MAX_CLASSIC_PACKET_BYTES) {
        return 0;
    }
    if (packet_size <= ISO_TP_SINGLE_DATA_BYTES) return 1;

    const size_t remaining = packet_size - ISO_TP_FIRST_DATA_BYTES;
    return 1 +
        (remaining + ISO_TP_CONSECUTIVE_DATA_BYTES - 1) /
            ISO_TP_CONSECUTIVE_DATA_BYTES;
}

bool As11IsoTpTransmitter::begin(const uint8_t *packet,
                                 size_t packet_size,
                                 As11IsoTpCanFrame &first_frame) {
    reset();
    if (!packet || packet_size == 0 ||
        packet_size > ISO_TP_MAX_CLASSIC_PACKET_BYTES) {
        return false;
    }

    packet_ = packet;
    packet_size_ = packet_size;
    first_frame = {};

    if (packet_size <= ISO_TP_SINGLE_DATA_BYTES) {
        first_frame.len = static_cast<uint8_t>(packet_size + 1);
        first_frame.data[0] = static_cast<uint8_t>(packet_size);
        memcpy(first_frame.data + 1, packet, packet_size);
        offset_ = packet_size;
        return true;
    }

    first_frame.len = 8;
    first_frame.data[0] =
        (static_cast<uint8_t>(As11IsoTpFrameType::First)
         << ISO_TP_PCI_SHIFT) |
        static_cast<uint8_t>((packet_size >> 8) & ISO_TP_PCI_VALUE_MASK);
    first_frame.data[1] = static_cast<uint8_t>(packet_size & 0xFF);
    memcpy(first_frame.data + 2, packet, ISO_TP_FIRST_DATA_BYTES);

    offset_ = ISO_TP_FIRST_DATA_BYTES;
    waiting_flow_control_ = true;
    return true;
}

bool As11IsoTpTransmitter::grant(
    const As11IsoTpFlowControl &flow_control) {
    if (!packet_ || complete() || !waiting_flow_control_ ||
        flow_control.status != As11IsoTpFlowStatus::Continue) {
        return false;
    }

    uint32_t delay_ms = 0;
    if (!as11_isotp_st_min_delay_ms(flow_control.st_min, delay_ms)) {
        return false;
    }

    block_remaining_ = flow_control.block_size == 0
        ? UINT16_MAX
        : flow_control.block_size;
    waiting_flow_control_ = false;
    return true;
}

As11IsoTpTransmitStatus As11IsoTpTransmitter::next(
    As11IsoTpCanFrame &frame) {
    frame = {};
    if (!packet_) return As11IsoTpTransmitStatus::Error;
    if (complete()) return As11IsoTpTransmitStatus::Complete;
    if (waiting_flow_control_) {
        return As11IsoTpTransmitStatus::WaitFlowControl;
    }

    const size_t chunk = std::min(ISO_TP_CONSECUTIVE_DATA_BYTES,
                                  packet_size_ - offset_);
    frame.len = static_cast<uint8_t>(chunk + 1);
    frame.data[0] =
        (static_cast<uint8_t>(As11IsoTpFrameType::Consecutive)
         << ISO_TP_PCI_SHIFT) |
        (next_sequence_ & ISO_TP_PCI_VALUE_MASK);
    memcpy(frame.data + 1, packet_ + offset_, chunk);

    offset_ += chunk;
    next_sequence_ = static_cast<uint8_t>((next_sequence_ + 1) &
                                          ISO_TP_PCI_VALUE_MASK);

    if (!complete() && block_remaining_ != UINT16_MAX) {
        block_remaining_--;
        if (block_remaining_ == 0) waiting_flow_control_ = true;
    }
    return As11IsoTpTransmitStatus::FrameReady;
}

void As11IsoTpTransmitter::reset() {
    packet_ = nullptr;
    packet_size_ = 0;
    offset_ = 0;
    block_remaining_ = 0;
    next_sequence_ = 1;
    waiting_flow_control_ = false;
}

bool As11IsoTpTransmitter::complete() const {
    return packet_ && offset_ >= packet_size_;
}

const char *as11_isotp_receive_error_name(As11IsoTpReceiveError error) {
    switch (error) {
        case As11IsoTpReceiveError::None: return "none";
        case As11IsoTpReceiveError::InvalidFrame: return "invalid_frame";
        case As11IsoTpReceiveError::UnexpectedFrame:
            return "unexpected_frame";
        case As11IsoTpReceiveError::SequenceMismatch:
            return "sequence_mismatch";
        case As11IsoTpReceiveError::Overflow: return "packet_overflow";
        case As11IsoTpReceiveError::Timeout: return "reassembly_timeout";
    }
    return "unknown";
}

As11IsoTpReceiveResult As11IsoTpReceiver::feed(
    const uint8_t *frame,
    size_t frame_size,
    uint8_t *packet,
    size_t packet_capacity,
    uint8_t block_size,
    uint32_t now_ms) {
    if (!packet) return fail(As11IsoTpReceiveError::Overflow);

    As11IsoTpFrameType type;
    if (!as11_isotp_frame_type(frame, frame_size, type)) {
        return fail(As11IsoTpReceiveError::InvalidFrame);
    }

    if (type == As11IsoTpFrameType::Single) {
        if (active_) return fail(As11IsoTpReceiveError::UnexpectedFrame);

        const size_t payload_size = frame[0] & ISO_TP_PCI_VALUE_MASK;
        if (payload_size == 0 || payload_size > ISO_TP_SINGLE_DATA_BYTES ||
            frame_size < payload_size + 1) {
            return fail(As11IsoTpReceiveError::InvalidFrame);
        }
        if (payload_size > packet_capacity) {
            return fail(As11IsoTpReceiveError::Overflow);
        }

        memcpy(packet, frame + 1, payload_size);
        As11IsoTpReceiveResult result;
        result.status = As11IsoTpReceiveStatus::Complete;
        result.packet_size = payload_size;
        return result;
    }

    if (type == As11IsoTpFrameType::First) {
        if (active_) return fail(As11IsoTpReceiveError::UnexpectedFrame);
        if (frame_size != 8) {
            return fail(As11IsoTpReceiveError::InvalidFrame);
        }

        const size_t expected_size =
            (static_cast<size_t>(frame[0] & ISO_TP_PCI_VALUE_MASK) << 8) |
            frame[1];
        if (expected_size <= ISO_TP_SINGLE_DATA_BYTES) {
            return fail(As11IsoTpReceiveError::InvalidFrame);
        }
        if (expected_size > packet_capacity) {
            return fail(As11IsoTpReceiveError::Overflow);
        }

        memcpy(packet, frame + 2, ISO_TP_FIRST_DATA_BYTES);
        expected_size_ = expected_size;
        packet_size_ = ISO_TP_FIRST_DATA_BYTES;
        last_frame_ms_ = now_ms;
        expected_sequence_ = 1;
        block_frames_ = 0;
        active_ = true;

        As11IsoTpReceiveResult result;
        result.status = As11IsoTpReceiveStatus::FlowControlNeeded;
        return result;
    }

    if (type != As11IsoTpFrameType::Consecutive || !active_) {
        return fail(As11IsoTpReceiveError::UnexpectedFrame);
    }
    if (frame_size < 2) {
        return fail(As11IsoTpReceiveError::InvalidFrame);
    }

    const uint8_t sequence = frame[0] & ISO_TP_PCI_VALUE_MASK;
    if (sequence != expected_sequence_) {
        return fail(As11IsoTpReceiveError::SequenceMismatch);
    }

    const size_t remaining = expected_size_ - packet_size_;
    const size_t required = std::min(ISO_TP_CONSECUTIVE_DATA_BYTES,
                                     remaining);
    if (frame_size - 1 < required) {
        return fail(As11IsoTpReceiveError::InvalidFrame);
    }

    memcpy(packet + packet_size_, frame + 1, required);
    packet_size_ += required;
    expected_sequence_ = static_cast<uint8_t>((expected_sequence_ + 1) &
                                              ISO_TP_PCI_VALUE_MASK);
    block_frames_++;
    last_frame_ms_ = now_ms;

    if (packet_size_ == expected_size_) {
        As11IsoTpReceiveResult result;
        result.status = As11IsoTpReceiveStatus::Complete;
        result.packet_size = packet_size_;
        reset();
        return result;
    }

    if (block_size != 0 && block_frames_ >= block_size) {
        block_frames_ = 0;
        As11IsoTpReceiveResult result;
        result.status = As11IsoTpReceiveStatus::FlowControlNeeded;
        return result;
    }
    return {};
}

As11IsoTpReceiveResult As11IsoTpReceiver::poll(uint32_t now_ms,
                                               uint32_t timeout_ms) {
    if (!active_ || timeout_ms == 0 ||
        !millis_elapsed_at_least(now_ms, last_frame_ms_, timeout_ms)) {
        return {};
    }
    return fail(As11IsoTpReceiveError::Timeout);
}

void As11IsoTpReceiver::reset() {
    expected_size_ = 0;
    packet_size_ = 0;
    last_frame_ms_ = 0;
    expected_sequence_ = 1;
    block_frames_ = 0;
    active_ = false;
}

As11IsoTpReceiveResult As11IsoTpReceiver::fail(
    As11IsoTpReceiveError error) {
    reset();

    As11IsoTpReceiveResult result;
    result.status = As11IsoTpReceiveStatus::Error;
    result.error = error;
    return result;
}

}  // namespace aircannect
