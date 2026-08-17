#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t AC_RESMED_ABC_PRIMARY_BYTES = 0x58;
static constexpr size_t AC_RESMED_ABC_DESCRIPTOR_BYTES = 0x50;
static constexpr size_t AC_RESMED_ABC_0005_HEADER_BYTES =
    AC_RESMED_ABC_PRIMARY_BYTES + AC_RESMED_ABC_DESCRIPTOR_BYTES;
static constexpr size_t AC_RESMED_ABC_SEGMENT_BYTES = 8;
static constexpr size_t AC_RESMED_RAW_ABC_PREFIX_BYTES =
    AC_RESMED_ABC_0005_HEADER_BYTES + AC_RESMED_ABC_SEGMENT_BYTES;

enum class ResmedFirmwareImageKind : uint8_t {
    Unknown,
    Abc0005,
    Abc0006,
    Raw,
};

enum class ResmedFirmwareTarget : uint8_t {
    Conf,
    Appl,
    Apcx,
    Fgbl,
    Fgcb,
};

enum class ResmedFirmwareInstallTransport : uint8_t {
    Rpc,
    Service,
};

static constexpr ResmedFirmwareTarget AC_RESMED_FIRMWARE_DEFAULT_TARGET =
    ResmedFirmwareTarget::Apcx;
static constexpr ResmedFirmwareInstallTransport
    AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT =
        ResmedFirmwareInstallTransport::Rpc;

struct ResmedFirmwareImageInfo {
    ResmedFirmwareImageKind kind = ResmedFirmwareImageKind::Unknown;
    uint64_t input_size = 0;
    uint64_t prepared_size = 0;
    uint64_t source_offset = 0;
    uint64_t payload_size = 0;
    uint64_t service_source_offset = 0;
    uint64_t service_payload_size = 0;
    uint32_t flash_start = 0;
    uint32_t rest_crc = 0;
    uint32_t descriptor_word_2 = 0;
    uint32_t descriptor_word_3 = 0;
    char target[5] = {};
    char descriptor_version[16] = {};

    bool valid() const {
        return kind != ResmedFirmwareImageKind::Unknown &&
               input_size != 0 && prepared_size != 0 && target[0] != '\0';
    }

    bool passthrough() const {
        return kind == ResmedFirmwareImageKind::Abc0005 ||
               kind == ResmedFirmwareImageKind::Abc0006;
    }

    bool service_payload_valid() const {
        return service_payload_size != 0;
    }
};

const char *resmed_firmware_image_kind_name(ResmedFirmwareImageKind kind);
bool resmed_firmware_version_from_text(const char *text,
                                       char *out,
                                       size_t out_size);
const char *resmed_firmware_target_code(ResmedFirmwareTarget target);
bool resmed_firmware_target_parse(const char *code,
                                  ResmedFirmwareTarget &target);
const char *resmed_firmware_install_transport_name(
    ResmedFirmwareInstallTransport transport);
bool resmed_firmware_install_transport_parse(
    const char *name,
    ResmedFirmwareInstallTransport &transport);
bool resmed_firmware_target_range(ResmedFirmwareTarget target,
                                  uint32_t &flash_start,
                                  uint64_t &payload_size);

class ResmedFirmwareInspector {
public:
    bool begin(uint64_t input_size,
               const char *filename,
               const char *device_identifier,
               ResmedFirmwareTarget target,
               ResmedFirmwareInstallTransport transport =
                   AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT);
    bool consume(uint64_t offset, const uint8_t *data, size_t length);
    bool finish();

    const ResmedFirmwareImageInfo &info() const { return info_; }
    const char *error() const { return error_; }

private:
    bool configure_from_prefix();
    bool configure_raw();
    bool configure_abc_0005();
    bool configure_abc_0006();
    bool consume_configured(uint64_t offset,
                            const uint8_t *data,
                            size_t length);
    bool consume_abc(uint64_t offset,
                     const uint8_t *data,
                     size_t length);
    bool consume_raw(uint64_t offset,
                     const uint8_t *data,
                     size_t length);
    bool parse_abc_0005_header();
    bool parse_segment_byte(uint8_t value);
    bool fail(const char *error);

    ResmedFirmwareImageInfo info_;
    uint64_t received_ = 0;
    uint64_t payload_received_ = 0;
    uint64_t rest_received_ = 0;
    uint64_t segment_data_bytes_ = 0;
    uint32_t rest_crc_state_ = 0;
    uint32_t expected_rest_crc_ = 0;
    uint32_t segment_count_ = 0;
    uint32_t segments_parsed_ = 0;
    uint32_t segment_table_bytes_ = 0;
    uint32_t segment_partial_bytes_ = 0;
    uint64_t target_flash_end_ = 0;
    uint64_t target_payload_size_ = 0;
    uint32_t first_segment_start_ = 0;
    uint32_t first_segment_length_ = 0;
    size_t header_required_ = 0;
    size_t header_received_ = 0;
    bool configured_ = false;
    bool header_parsed_ = false;
    uint8_t header_[AC_RESMED_ABC_0005_HEADER_BYTES] = {};
    uint8_t segment_partial_[AC_RESMED_ABC_SEGMENT_BYTES] = {};
    char filename_[96] = {};
    char device_identifier_[96] = {};
    char error_[64] = {};
    ResmedFirmwareTarget requested_target_ =
        AC_RESMED_FIRMWARE_DEFAULT_TARGET;
    ResmedFirmwareInstallTransport transport_ =
        AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT;
};

bool resmed_build_raw_abc_prefix(
    const ResmedFirmwareImageInfo &info,
    uint8_t out[AC_RESMED_RAW_ABC_PREFIX_BYTES]);

}  // namespace aircannect
