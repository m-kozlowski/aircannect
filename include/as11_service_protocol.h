#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr uint8_t AS11_SERVICE_PACKET_MAGIC = 0xA5;
static constexpr uint8_t AS11_SERVICE_PROTOCOL_VERSION_V1 = 1;
static constexpr uint8_t AS11_SERVICE_PROTOCOL_VERSION_V2 = 2;
static constexpr uint8_t AS11_SERVICE_PROTOCOL_VERSION_CURRENT =
    AS11_SERVICE_PROTOCOL_VERSION_V2;
static constexpr size_t AS11_SERVICE_PACKET_HEADER_BYTES = 8;
static constexpr size_t AS11_SERVICE_V1_CRC_BYTES = 4;
static constexpr size_t AS11_SERVICE_V2_CRC_BYTES = 2;
static constexpr size_t AS11_SERVICE_PACKET_MAX_BYTES = 0x0FFF;
static constexpr size_t AS11_SERVICE_V1_PAYLOAD_MAX_BYTES =
    AS11_SERVICE_PACKET_MAX_BYTES - AS11_SERVICE_PACKET_HEADER_BYTES -
    AS11_SERVICE_V1_CRC_BYTES;
static constexpr size_t AS11_SERVICE_V2_PAYLOAD_MAX_BYTES =
    AS11_SERVICE_PACKET_MAX_BYTES - AS11_SERVICE_PACKET_HEADER_BYTES -
    AS11_SERVICE_V2_CRC_BYTES;

// Local host-transport extension. ENTER is consumed by AirCANnect and is
// never forwarded to the AS11 service CAN IDs.
static constexpr uint8_t AS11_SERVICE_COMMAND_ENTER = 0x01;
static constexpr uint8_t AS11_SERVICE_COMMAND_INFO = 0x02;
static constexpr uint8_t AS11_SERVICE_COMMAND_ERASE = 0x04;
static constexpr uint8_t AS11_SERVICE_COMMAND_WRITE = 0x05;
static constexpr uint8_t AS11_SERVICE_COMMAND_RESET = 0x06;
static constexpr uint8_t AS11_SERVICE_STATUS_OK = 0x00;
static constexpr uint8_t AS11_SERVICE_STATUS_ENTRY_TIMEOUT = 0x0B;
static constexpr uint8_t AS11_SERVICE_TARGET_FGCB = 0x01;
static constexpr size_t AS11_SERVICE_STORAGE_ADDRESS_BYTES = 5;
static constexpr size_t AS11_SERVICE_V2_WRITE_DATA_MAX_BYTES = 4064;

static constexpr uint8_t AS11_SERVICE_RESPONSE_BLOCK_SIZE = 128;
static constexpr uint8_t AS11_SERVICE_RESPONSE_ST_MIN = 0;

struct As11ServicePacketHeader {
    uint8_t protocol_version = 0;
    uint8_t command = 0;
    uint8_t status = 0;
    uint16_t sequence = 0;
    uint16_t payload_length = 0;
};

enum class As11ServicePacketError : uint8_t {
    None,
    TooShort,
    TooLarge,
    BadMagic,
    UnsupportedVersion,
    LengthMismatch,
    CrcMismatch,
};

const char *as11_service_packet_error_name(As11ServicePacketError error);

size_t as11_service_packet_crc_bytes(uint8_t protocol_version);

As11ServicePacketError as11_service_packet_size_from_header(
    const uint8_t *packet_header,
    size_t header_size,
    size_t &packet_size);

As11ServicePacketError as11_service_validate_packet(
    const uint8_t *packet,
    size_t packet_size,
    As11ServicePacketHeader &header);

bool as11_service_encode_packet(uint8_t protocol_version,
                                uint8_t command,
                                uint8_t status,
                                uint16_t sequence,
                                const uint8_t *payload,
                                size_t payload_size,
                                uint8_t *packet,
                                size_t packet_capacity,
                                size_t &packet_size);

enum class As11IsoTpFrameType : uint8_t {
    Single = 0,
    First = 1,
    Consecutive = 2,
    FlowControl = 3,
};

enum class As11IsoTpFlowStatus : uint8_t {
    Continue = 0,
    Wait = 1,
    Overflow = 2,
};

struct As11IsoTpCanFrame {
    uint8_t len = 0;
    uint8_t data[8] = {};
};

struct As11IsoTpFlowControl {
    As11IsoTpFlowStatus status = As11IsoTpFlowStatus::Continue;
    uint8_t block_size = 0;
    uint8_t st_min = 0;
};

bool as11_isotp_frame_type(const uint8_t *frame,
                           size_t frame_size,
                           As11IsoTpFrameType &type);

bool as11_isotp_parse_flow_control(const uint8_t *frame,
                                   size_t frame_size,
                                   As11IsoTpFlowControl &flow_control);

bool as11_isotp_make_flow_control(uint8_t block_size,
                                  uint8_t st_min,
                                  As11IsoTpCanFrame &frame);

bool as11_isotp_st_min_delay_ms(uint8_t st_min, uint32_t &delay_ms);

size_t as11_isotp_frame_count(size_t packet_size);

enum class As11IsoTpTransmitStatus : uint8_t {
    FrameReady,
    WaitFlowControl,
    Complete,
    Error,
};

class As11IsoTpTransmitter {
public:
    bool begin(const uint8_t *packet,
               size_t packet_size,
               As11IsoTpCanFrame &first_frame);
    bool grant(const As11IsoTpFlowControl &flow_control);
    As11IsoTpTransmitStatus next(As11IsoTpCanFrame &frame);
    void reset();

    bool complete() const;
    bool waiting_flow_control() const { return waiting_flow_control_; }

private:
    const uint8_t *packet_ = nullptr;
    size_t packet_size_ = 0;
    size_t offset_ = 0;
    uint16_t block_remaining_ = 0;
    uint8_t next_sequence_ = 1;
    bool waiting_flow_control_ = false;
};

enum class As11IsoTpReceiveStatus : uint8_t {
    InProgress,
    FlowControlNeeded,
    Complete,
    Error,
};

enum class As11IsoTpReceiveError : uint8_t {
    None,
    InvalidFrame,
    UnexpectedFrame,
    SequenceMismatch,
    Overflow,
    Timeout,
};

struct As11IsoTpReceiveResult {
    As11IsoTpReceiveStatus status = As11IsoTpReceiveStatus::InProgress;
    As11IsoTpReceiveError error = As11IsoTpReceiveError::None;
    size_t packet_size = 0;
};

const char *as11_isotp_receive_error_name(As11IsoTpReceiveError error);

class As11IsoTpReceiver {
public:
    As11IsoTpReceiveResult feed(const uint8_t *frame,
                                size_t frame_size,
                                uint8_t *packet,
                                size_t packet_capacity,
                                uint8_t block_size,
                                uint32_t now_ms);
    As11IsoTpReceiveResult poll(uint32_t now_ms, uint32_t timeout_ms);
    void reset();

private:
    As11IsoTpReceiveResult fail(As11IsoTpReceiveError error);

    size_t expected_size_ = 0;
    size_t packet_size_ = 0;
    uint32_t last_frame_ms_ = 0;
    uint8_t expected_sequence_ = 1;
    uint8_t block_frames_ = 0;
    bool active_ = false;
};

}  // namespace aircannect
