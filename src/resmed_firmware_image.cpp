#include "resmed_firmware_image.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"

namespace aircannect {
namespace {

constexpr uint64_t FullFlashBytes = 0x00200000;
constexpr uint64_t ConfigBytes = 0x00020000;
constexpr uint64_t ApplicationBytes = 0x001C0000;
constexpr uint64_t ConfigAndApplicationBytes = 0x001E0000;
constexpr uint32_t FlashBase = 0x08000000;
constexpr uint32_t ConfigFlashStart = 0x08020000;
constexpr uint32_t ApplicationFlashStart = 0x08040000;
constexpr char Component0005[] = "PacificFG";

struct FirmwareTargetSpec {
    ResmedFirmwareTarget id = AC_RESMED_FIRMWARE_DEFAULT_TARGET;
    const char *code = nullptr;
    uint32_t flash_start = 0;
    uint64_t payload_size = 0;
    bool descriptor_word_2 = false;
    bool descriptor_word_3 = false;
};

struct DescriptorPreset {
    const char *version = nullptr;
    uint32_t word_2 = 0;
    uint32_t word_3 = 0;
};

constexpr FirmwareTargetSpec FirmwareTargets[] = {
    {ResmedFirmwareTarget::Conf, "CONF", ConfigFlashStart, ConfigBytes,
     true, false},
    {ResmedFirmwareTarget::Appl, "APPL", ApplicationFlashStart,
     ApplicationBytes, true, true},
    {ResmedFirmwareTarget::Apcx, "APCX", ConfigFlashStart,
     ConfigAndApplicationBytes, false, true},
    {ResmedFirmwareTarget::Fgbl, "FGBL", FlashBase, ConfigBytes,
     false, true},
    {ResmedFirmwareTarget::Fgcb, "FGCB", FlashBase, FullFlashBytes,
     false, false},
};

// Keep presets ordered from oldest to newest. Combined CONF+APPL images may
// use the latest known desc3 because firmware does not validate desc2 there.
constexpr DescriptorPreset DescriptorPresets[] = {
    {"14.8.3.0", 0x2D89E58Fu, 0xBEB37EE2u},
    {"15.8.4.0", 0xD785ABA6u, 0xBEB37EE2u},
    {"16.8.5.0", 0x7862CBA7u, 0xBEB37EE2u},
    {"17.8.6.0", 0xBECBC5BCu, 0xBEB37EE2u},
};

constexpr size_t DescriptorPresetCount =
    sizeof(DescriptorPresets) / sizeof(DescriptorPresets[0]);
static_assert(DescriptorPresetCount > 0, "descriptor presets required");

uint32_t get_le32(const uint8_t *data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void put_le32(uint8_t *data, size_t offset, uint32_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
    data[offset + 2] = static_cast<uint8_t>(value >> 16);
    data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void copy_text(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s", value ? value : "");
}

bool extract_version(const char *text, char *out, size_t out_size) {
    if (!text || !out || out_size == 0) return false;
    out[0] = '\0';

    for (size_t i = 0; text[i]; ++i) {
        if (!isdigit(static_cast<unsigned char>(text[i]))) continue;

        int a = 0;
        int b = 0;
        int c = 0;
        int d = 0;
        int consumed = 0;
        if (sscanf(text + i, "%d.%d.%d.%d%n",
                   &a, &b, &c, &d, &consumed) != 4) {
            continue;
        }
        if (a <= 0 || a >= 100 || b < 0 || b >= 100 ||
            c < 0 || c >= 100 || d < 0 || d >= 100) {
            continue;
        }

        snprintf(out, out_size, "%d.%d.%d.%d", a, b, c, d);
        return true;
    }
    return false;
}

bool descriptor_preset(const char *version,
                       uint32_t &word_2,
                       uint32_t &word_3) {
    for (const DescriptorPreset &preset : DescriptorPresets) {
        if (version && strcmp(version, preset.version) == 0) {
            word_2 = preset.word_2;
            word_3 = preset.word_3;
            return true;
        }
    }
    return false;
}

const DescriptorPreset &latest_descriptor_preset() {
    return DescriptorPresets[DescriptorPresetCount - 1];
}

const FirmwareTargetSpec *target_spec(ResmedFirmwareTarget target) {
    for (const FirmwareTargetSpec &candidate : FirmwareTargets) {
        if (candidate.id == target) return &candidate;
    }
    return nullptr;
}

const FirmwareTargetSpec *target_spec(const char code[5]) {
    if (!code) return nullptr;
    for (const FirmwareTargetSpec &candidate : FirmwareTargets) {
        if (strncmp(code, candidate.code, 4) == 0) return &candidate;
    }
    return nullptr;
}

bool ascii_target_code(const uint8_t *code) {
    for (size_t i = 0; i < 4; ++i) {
        if (!isupper(static_cast<unsigned char>(code[i])) &&
            !isdigit(static_cast<unsigned char>(code[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char *resmed_firmware_image_kind_name(ResmedFirmwareImageKind kind) {
    switch (kind) {
        case ResmedFirmwareImageKind::Abc0005: return "abc-0005";
        case ResmedFirmwareImageKind::Abc0006: return "abc-0006";
        case ResmedFirmwareImageKind::Raw: return "raw";
        case ResmedFirmwareImageKind::Unknown: return "unknown";
    }
    return "unknown";
}

const char *resmed_firmware_target_code(ResmedFirmwareTarget target) {
    const FirmwareTargetSpec *spec = target_spec(target);
    return spec ? spec->code : "APCX";
}

bool resmed_firmware_target_parse(const char *code,
                                  ResmedFirmwareTarget &target) {
    if (!code || strlen(code) != 4) return false;

    char normalized[5] = {};
    for (size_t i = 0; i < 4; ++i) {
        normalized[i] = static_cast<char>(
            toupper(static_cast<unsigned char>(code[i])));
    }

    const FirmwareTargetSpec *spec = target_spec(normalized);
    if (!spec) return false;
    target = spec->id;
    return true;
}

const char *resmed_firmware_install_transport_name(
    ResmedFirmwareInstallTransport transport) {
    switch (transport) {
        case ResmedFirmwareInstallTransport::Rpc: return "rpc";
        case ResmedFirmwareInstallTransport::Service: return "service";
    }
    return "rpc";
}

bool resmed_firmware_install_transport_parse(
    const char *name,
    ResmedFirmwareInstallTransport &transport) {
    if (!name) return false;

    if (strcasecmp(name, "rpc") == 0) {
        transport = ResmedFirmwareInstallTransport::Rpc;
        return true;
    }
    if (strcasecmp(name, "service") == 0) {
        transport = ResmedFirmwareInstallTransport::Service;
        return true;
    }
    return false;
}

bool ResmedFirmwareInspector::begin(uint64_t input_size,
                                    const char *filename,
                                    const char *device_identifier,
                                    ResmedFirmwareTarget target,
                                    ResmedFirmwareInstallTransport transport) {
    *this = ResmedFirmwareInspector();
    if (input_size == 0) return fail("empty_image");
    if (!target_spec(target)) return fail("unsupported_target");

    info_.input_size = input_size;
    copy_text(filename_, sizeof(filename_), filename);
    copy_text(device_identifier_, sizeof(device_identifier_),
              device_identifier);
    requested_target_ = target;
    transport_ = transport;
    rest_crc_state_ = crc32_ieee_initial_state();
    return true;
}

bool ResmedFirmwareInspector::consume(uint64_t offset,
                                      const uint8_t *data,
                                      size_t length) {
    if (error_[0]) return false;
    if ((!data && length) || offset != received_ ||
        length > info_.input_size - received_) {
        return fail("input_offset_mismatch");
    }
    if (length == 0) return true;

    size_t consumed = 0;
    if (!configured_) {
        const size_t needed = 8 - header_received_;
        const size_t prefix_bytes = std::min(needed, length);
        memcpy(header_ + header_received_, data, prefix_bytes);
        header_received_ += prefix_bytes;
        consumed += prefix_bytes;

        if (header_received_ == 8) {
            if (!configure_from_prefix()) return false;

            header_received_ = 0;
            if (!consume_configured(0, header_, 8)) {
                return false;
            }
        }
    }

    if (configured_ && consumed < length &&
        !consume_configured(offset + consumed, data + consumed,
                            length - consumed)) {
        return false;
    }

    received_ += length;
    return true;
}

bool ResmedFirmwareInspector::configure_from_prefix() {
    if (memcmp(header_, "OTA!", 4) != 0) return configure_raw();
    if (memcmp(header_ + 4, "0005", 4) == 0) {
        return configure_abc_0005();
    }
    if (memcmp(header_ + 4, "0006", 4) == 0) {
        return configure_abc_0006();
    }
    return fail("unsupported_abc_format");
}

bool ResmedFirmwareInspector::configure_raw() {
    const FirmwareTargetSpec *target = target_spec(requested_target_);
    if (!target) return fail("unsupported_target");

    if (info_.input_size == target->payload_size) {
        info_.source_offset = 0;
    } else if (info_.input_size == FullFlashBytes) {
        info_.source_offset = target->flash_start - FlashBase;
    } else {
        return fail("raw_image_size_mismatch");
    }

    uint32_t word_2 = 0;
    uint32_t word_3 = 0;
    bool have_preset = false;
    if (extract_version(device_identifier_, info_.descriptor_version,
                        sizeof(info_.descriptor_version))) {
        have_preset = descriptor_preset(info_.descriptor_version,
                                        word_2, word_3);
    }
    if (!have_preset &&
        extract_version(filename_, info_.descriptor_version,
                        sizeof(info_.descriptor_version))) {
        have_preset = descriptor_preset(info_.descriptor_version,
                                        word_2, word_3);
    }
    if (!have_preset) {
        if (transport_ == ResmedFirmwareInstallTransport::Rpc &&
            requested_target_ != ResmedFirmwareTarget::Apcx &&
            requested_target_ != ResmedFirmwareTarget::Fgcb) {
            return fail("unsupported_descriptor_preset");
        }

        word_2 = 0;
        word_3 = 0;
        if (transport_ == ResmedFirmwareInstallTransport::Rpc &&
            requested_target_ == ResmedFirmwareTarget::Apcx) {
            word_3 = latest_descriptor_preset().word_3;
        }
    }

    info_.kind = ResmedFirmwareImageKind::Raw;
    info_.payload_size = target->payload_size;
    info_.service_source_offset = info_.source_offset;
    info_.service_payload_size = target->payload_size;
    info_.prepared_size = AC_RESMED_RAW_ABC_PREFIX_BYTES +
                          target->payload_size;
    info_.flash_start = target->flash_start;
    if (target->descriptor_word_2) info_.descriptor_word_2 = word_2;
    if (target->descriptor_word_3) info_.descriptor_word_3 = word_3;
    copy_text(info_.target, sizeof(info_.target), target->code);

    uint8_t segment[AC_RESMED_ABC_SEGMENT_BYTES] = {};
    put_le32(segment, 0, static_cast<uint32_t>(target->payload_size));
    put_le32(segment, 4, target->flash_start);
    rest_crc_state_ = crc32_ieee_update_state(
        crc32_ieee_initial_state(), segment, sizeof(segment));
    configured_ = true;
    return true;
}

bool ResmedFirmwareInspector::configure_abc_0005() {
    if (info_.input_size < AC_RESMED_ABC_0005_HEADER_BYTES) {
        return fail("abc_0005_too_short");
    }

    info_.kind = ResmedFirmwareImageKind::Abc0005;
    info_.prepared_size = info_.input_size;
    header_required_ = AC_RESMED_ABC_0005_HEADER_BYTES;
    configured_ = true;
    return true;
}

bool ResmedFirmwareInspector::configure_abc_0006() {
    if (info_.input_size != AC_RESMED_ABC_PRIMARY_BYTES + FullFlashBytes) {
        return fail("abc_0006_bad_size");
    }

    if (requested_target_ != ResmedFirmwareTarget::Fgcb) {
        return fail("image_target_mismatch");
    }

    info_.kind = ResmedFirmwareImageKind::Abc0006;
    info_.prepared_size = info_.input_size;
    info_.source_offset = AC_RESMED_ABC_PRIMARY_BYTES;
    info_.payload_size = FullFlashBytes;
    info_.service_source_offset = AC_RESMED_ABC_PRIMARY_BYTES;
    info_.service_payload_size = FullFlashBytes;
    info_.flash_start = FlashBase;
    copy_text(info_.target, sizeof(info_.target), "FGCB");
    header_required_ = AC_RESMED_ABC_PRIMARY_BYTES;
    configured_ = true;
    return true;
}

bool ResmedFirmwareInspector::consume_configured(uint64_t offset,
                                                 const uint8_t *data,
                                                 size_t length) {
    if (info_.kind == ResmedFirmwareImageKind::Raw) {
        return consume_raw(offset, data, length);
    }
    return consume_abc(offset, data, length);
}

bool ResmedFirmwareInspector::consume_raw(uint64_t offset,
                                          const uint8_t *data,
                                          size_t length) {
    const uint64_t input_end = offset + length;
    const uint64_t payload_start = info_.source_offset;
    const uint64_t payload_end = payload_start + info_.payload_size;
    if (input_end <= payload_start || offset >= payload_end) return true;

    const uint64_t copy_start = std::max(offset, payload_start);
    const uint64_t copy_end = std::min(input_end, payload_end);
    const size_t data_offset = static_cast<size_t>(copy_start - offset);
    const size_t copy_length = static_cast<size_t>(copy_end - copy_start);
    rest_crc_state_ = crc32_ieee_update_state(
        rest_crc_state_, data + data_offset, copy_length);
    payload_received_ += copy_length;
    return true;
}

bool ResmedFirmwareInspector::consume_abc(uint64_t offset,
                                          const uint8_t *data,
                                          size_t length) {
    const uint64_t input_end = offset + length;
    if (offset < header_required_) {
        const uint64_t copy_end = std::min<uint64_t>(input_end,
                                                     header_required_);
        const size_t copy_length = static_cast<size_t>(copy_end - offset);
        if (offset != header_received_) return fail("abc_header_gap");
        memcpy(header_ + offset, data, copy_length);
        header_received_ += copy_length;
    }

    if (header_received_ == header_required_ && !header_parsed_) {
        if (info_.kind == ResmedFirmwareImageKind::Abc0005) {
            if (!parse_abc_0005_header()) return false;
        } else {
            header_parsed_ = true;
        }
    }

    if (input_end <= header_required_) return true;
    if (!header_parsed_) return fail("abc_header_incomplete");

    const uint64_t rest_start = std::max<uint64_t>(offset,
                                                   header_required_);
    const size_t data_offset = static_cast<size_t>(rest_start - offset);
    const size_t rest_length = static_cast<size_t>(input_end - rest_start);
    const uint8_t *rest = data + data_offset;

    if (info_.kind == ResmedFirmwareImageKind::Abc0005) {
        rest_crc_state_ = crc32_ieee_update_state(rest_crc_state_, rest,
                                                  rest_length);
        for (size_t i = 0;
             i < rest_length &&
             rest_received_ + i < segment_table_bytes_;
             ++i) {
            if (!parse_segment_byte(rest[i])) return false;
        }
        rest_received_ += rest_length;
    } else {
        payload_received_ += rest_length;
    }
    return true;
}

bool ResmedFirmwareInspector::parse_abc_0005_header() {
    const uint8_t *descriptor = header_ + AC_RESMED_ABC_PRIMARY_BYTES;
    if (get_le32(descriptor, 0) != 1) return fail("abc_bad_marker");
    if (!ascii_target_code(descriptor + 4)) {
        return fail("abc_bad_target");
    }

    memcpy(info_.target, descriptor + 4, 4);
    info_.target[4] = '\0';
    const FirmwareTargetSpec *target = target_spec(info_.target);
    if (!target) {
        return fail("abc_unsupported_target");
    }
    if (target->id != requested_target_) {
        return fail("image_target_mismatch");
    }

    info_.flash_start = target->flash_start;
    target_flash_end_ = static_cast<uint64_t>(target->flash_start) +
                        target->payload_size;
    target_payload_size_ = target->payload_size;

    const uint32_t expected_rest_size = get_le32(descriptor, 0x40);
    expected_rest_crc_ = get_le32(descriptor, 0x44);
    segment_count_ = get_le32(descriptor, 0x48);
    if (segment_count_ == 0 || segment_count_ > 255) {
        return fail("abc_bad_segment_count");
    }
    segment_table_bytes_ = segment_count_ * AC_RESMED_ABC_SEGMENT_BYTES;
    if (segment_table_bytes_ > expected_rest_size) {
        return fail("abc_segment_table_truncated");
    }
    if (info_.input_size != AC_RESMED_ABC_0005_HEADER_BYTES +
                                expected_rest_size) {
        return fail("abc_length_mismatch");
    }

    const uint32_t expected_descriptor_crc = get_le32(descriptor, 0x4C);
    uint32_t descriptor_crc = crc32_ieee_initial_state();
    descriptor_crc = crc32_ieee_update_state(
        descriptor_crc, header_, AC_RESMED_ABC_PRIMARY_BYTES);
    descriptor_crc = crc32_ieee_update_state(
        descriptor_crc, descriptor, 0x4C);
    descriptor_crc = crc32_ieee_finish_state(descriptor_crc);
    if (descriptor_crc != expected_descriptor_crc) {
        return fail("abc_descriptor_crc_mismatch");
    }

    info_.payload_size = expected_rest_size;
    info_.source_offset = AC_RESMED_ABC_0005_HEADER_BYTES;
    rest_crc_state_ = crc32_ieee_initial_state();
    header_parsed_ = true;
    return true;
}

bool ResmedFirmwareInspector::parse_segment_byte(uint8_t value) {
    segment_partial_[segment_partial_bytes_++] = value;
    if (segment_partial_bytes_ != AC_RESMED_ABC_SEGMENT_BYTES) return true;

    const uint32_t length = get_le32(segment_partial_, 0);
    const uint32_t start = get_le32(segment_partial_, 4);
    const uint64_t end = static_cast<uint64_t>(start) + length;
    if (length == 0 || start < info_.flash_start ||
        end > target_flash_end_) {
        return fail("abc_segment_out_of_target");
    }
    if (segment_data_bytes_ > UINT64_MAX - length) {
        return fail("abc_segment_size_overflow");
    }

    if (segments_parsed_ == 0) {
        first_segment_start_ = start;
        first_segment_length_ = length;
    }
    segment_data_bytes_ += length;
    segments_parsed_++;
    segment_partial_bytes_ = 0;
    return true;
}

bool ResmedFirmwareInspector::finish() {
    if (error_[0]) return false;
    if (!configured_ || received_ != info_.input_size) {
        return fail("input_incomplete");
    }

    if (info_.kind == ResmedFirmwareImageKind::Raw) {
        if (payload_received_ != info_.payload_size) {
            return fail("raw_payload_incomplete");
        }
        info_.rest_crc = crc32_ieee_finish_state(rest_crc_state_);
        return true;
    }

    if (!header_parsed_) return fail("abc_header_incomplete");
    if (info_.kind == ResmedFirmwareImageKind::Abc0006) {
        return payload_received_ == info_.payload_size
            ? true
            : fail("abc_payload_incomplete");
    }

    if (rest_received_ != info_.payload_size ||
        segments_parsed_ != segment_count_ || segment_partial_bytes_ != 0) {
        return fail("abc_segment_table_incomplete");
    }
    const uint64_t data_bytes = info_.payload_size - segment_table_bytes_;
    if (segment_data_bytes_ != data_bytes) {
        return fail("abc_segment_length_mismatch");
    }
    if (crc32_ieee_finish_state(rest_crc_state_) != expected_rest_crc_) {
        return fail("abc_payload_crc_mismatch");
    }
    info_.rest_crc = expected_rest_crc_;

    if (segment_count_ == 1 && first_segment_start_ == info_.flash_start &&
        first_segment_length_ == target_payload_size_) {
        info_.service_source_offset =
            AC_RESMED_ABC_0005_HEADER_BYTES + segment_table_bytes_;
        info_.service_payload_size = segment_data_bytes_;
    }
    return true;
}

bool ResmedFirmwareInspector::fail(const char *error) {
    copy_text(error_, sizeof(error_), error ? error : "invalid_image");
    return false;
}

bool resmed_build_raw_abc_prefix(
    const ResmedFirmwareImageInfo &info,
    uint8_t out[AC_RESMED_RAW_ABC_PREFIX_BYTES]) {
    if (!out || info.kind != ResmedFirmwareImageKind::Raw ||
        !info.valid() || info.payload_size > UINT32_MAX) {
        return false;
    }

    memset(out, 0, AC_RESMED_RAW_ABC_PREFIX_BYTES);
    memcpy(out, "OTA!", 4);
    memcpy(out + 4, "0005", 4);
    memcpy(out + 0x48, Component0005, strlen(Component0005));

    uint8_t *descriptor = out + AC_RESMED_ABC_PRIMARY_BYTES;
    put_le32(descriptor, 0x00, 1);
    memcpy(descriptor + 0x04, info.target, 4);
    put_le32(descriptor, 0x08, info.descriptor_word_2);
    put_le32(descriptor, 0x0C, info.descriptor_word_3);
    put_le32(descriptor, 0x10, 0);
    put_le32(descriptor, 0x40,
             static_cast<uint32_t>(AC_RESMED_ABC_SEGMENT_BYTES +
                                   info.payload_size));
    put_le32(descriptor, 0x44, info.rest_crc);
    put_le32(descriptor, 0x48, 1);

    uint32_t descriptor_crc = crc32_ieee_initial_state();
    descriptor_crc = crc32_ieee_update_state(
        descriptor_crc, out, AC_RESMED_ABC_PRIMARY_BYTES);
    descriptor_crc = crc32_ieee_update_state(
        descriptor_crc, descriptor, 0x4C);
    put_le32(descriptor, 0x4C,
             crc32_ieee_finish_state(descriptor_crc));

    uint8_t *segment = out + AC_RESMED_ABC_0005_HEADER_BYTES;
    put_le32(segment, 0, static_cast<uint32_t>(info.payload_size));
    put_le32(segment, 4, info.flash_start);
    return true;
}

}  // namespace aircannect
