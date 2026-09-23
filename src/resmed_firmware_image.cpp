#include "resmed_firmware_image.h"

#include <algorithm>
#include <ctype.h>
#include <limits.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "large_allocator.h"
#include "little_endian.h"

namespace aircannect {
namespace {

constexpr uint64_t FullFlashBytes = 0x00200000;
constexpr uint64_t ConfigBytes = 0x00020000;
constexpr uint64_t ApplicationBytes = 0x001C0000;
constexpr uint64_t ConfigAndApplicationBytes = 0x001E0000;
constexpr uint64_t MiniFullFlashBytes = 0x00100000;
constexpr uint64_t MiniApplicationBytes = 0x000C0000;
constexpr uint64_t MiniConfigAndApplicationBytes = 0x000E0000;
constexpr uint32_t FlashBase = 0x08000000;
constexpr uint32_t ConfigFlashStart = 0x08020000;
constexpr uint32_t ApplicationFlashStart = 0x08040000;
constexpr size_t MiniSegmentAlignment = 4;
constexpr size_t MiniSparseMergeGapBytes = 8;
constexpr char AirSense11Component0005[] = "PacificFG";
constexpr char AirMiniComponent0005[] = "MonacoFG";

struct FirmwareTargetSpec {
    ResmedFirmwareTarget id = AC_RESMED_FIRMWARE_DEFAULT_TARGET;
    const char *code = nullptr;
    uint32_t flash_start = 0;
    uint64_t airsense11_payload_size = 0;
    uint64_t airmini_payload_size = 0;
    bool descriptor_word_2 = false;
    bool descriptor_word_3 = false;
};

struct DescriptorPreset {
    const char *version = nullptr;
    uint32_t word_2 = 0;
    uint32_t word_3 = 0;
};

constexpr FirmwareTargetSpec FirmwareTargets[] = {
    {ResmedFirmwareTarget::Conf, "CONF", ConfigFlashStart,
     ConfigBytes, ConfigBytes, true, false},
    {ResmedFirmwareTarget::Appl, "APPL", ApplicationFlashStart,
     ApplicationBytes, MiniApplicationBytes, true, true},
    {ResmedFirmwareTarget::Apcx, "APCX", ConfigFlashStart,
     ConfigAndApplicationBytes, MiniConfigAndApplicationBytes, false, true},
    {ResmedFirmwareTarget::Fgbl, "FGBL", FlashBase,
     ConfigBytes, ConfigBytes, false, true},
    {ResmedFirmwareTarget::Fgcb, "FGCB", FlashBase,
     FullFlashBytes, MiniFullFlashBytes, false, false},
};

// Keep presets ordered from oldest to newest. Combined CONF+APPL images may
// use the latest known desc3 because firmware does not validate desc2 there.
constexpr DescriptorPreset AirSense11DescriptorPresets[] = {
    {"14.8.3.0", 0x2D89E58Fu, 0xBEB37EE2u},
    {"15.8.4.0", 0xD785ABA6u, 0xBEB37EE2u},
    {"16.8.5.0", 0x7862CBA7u, 0xBEB37EE2u},
    {"17.8.6.0", 0xBECBC5BCu, 0xBEB37EE2u},
};

// This pair is established only for the reference SW03900 release. Do not
// use it as a fallback for other AirMini application versions.
constexpr DescriptorPreset AirMiniDescriptorPresets[] = {
    {"1.4.0.3", 0xC907DAE6u, 0x09ABCDEFu},
};

uint32_t get_le32(const uint8_t *data, size_t offset) {
    return LittleEndian::get_le32(data + offset);
}

void put_le32(uint8_t *data, size_t offset, uint32_t value) {
    LittleEndian::put_le32(data + offset, value);
}

void copy_text(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "%s", value ? value : "");
}

bool descriptor_preset(ResmedFirmwareImageProfile profile,
                       const char *version,
                       uint32_t &word_2,
                       uint32_t &word_3) {
    const DescriptorPreset *presets = nullptr;
    size_t count = 0;
    if (profile == ResmedFirmwareImageProfile::AirMini) {
        presets = AirMiniDescriptorPresets;
        count = sizeof(AirMiniDescriptorPresets) /
                sizeof(AirMiniDescriptorPresets[0]);
    } else {
        presets = AirSense11DescriptorPresets;
        count = sizeof(AirSense11DescriptorPresets) /
                sizeof(AirSense11DescriptorPresets[0]);
    }

    for (size_t i = 0; i < count; ++i) {
        if (version && strcmp(version, presets[i].version) == 0) {
            word_2 = presets[i].word_2;
            word_3 = presets[i].word_3;
            return true;
        }
    }
    return false;
}

const DescriptorPreset &latest_descriptor_preset(
    ResmedFirmwareImageProfile profile) {
    if (profile == ResmedFirmwareImageProfile::AirMini) {
        return AirMiniDescriptorPresets[0];
    }
    constexpr size_t count = sizeof(AirSense11DescriptorPresets) /
                             sizeof(AirSense11DescriptorPresets[0]);
    return AirSense11DescriptorPresets[count - 1];
}

const FirmwareTargetSpec *target_spec(ResmedFirmwareTarget target) {
    constexpr size_t count = sizeof(FirmwareTargets) /
                             sizeof(FirmwareTargets[0]);
    for (size_t i = 0; i < count; ++i) {
        if (FirmwareTargets[i].id == target) return &FirmwareTargets[i];
    }
    return nullptr;
}

const FirmwareTargetSpec *target_spec(const char code[5]) {
    if (!code) return nullptr;
    constexpr size_t count = sizeof(FirmwareTargets) /
                             sizeof(FirmwareTargets[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strncmp(code, FirmwareTargets[i].code, 4) == 0) {
            return &FirmwareTargets[i];
        }
    }
    return nullptr;
}

uint64_t target_payload_size(ResmedFirmwareImageProfile profile,
                             const FirmwareTargetSpec &target) {
    return profile == ResmedFirmwareImageProfile::AirMini
        ? target.airmini_payload_size
        : target.airsense11_payload_size;
}

uint64_t profile_full_flash_bytes(ResmedFirmwareImageProfile profile) {
    return profile == ResmedFirmwareImageProfile::AirMini
        ? MiniFullFlashBytes
        : FullFlashBytes;
}

const char *component_name(ResmedFirmwareImageProfile profile) {
    return profile == ResmedFirmwareImageProfile::AirMini
        ? AirMiniComponent0005
        : AirSense11Component0005;
}

uint32_t gf2_matrix_times(const uint32_t *matrix, uint32_t vector) {
    uint32_t result = 0;
    size_t index = 0;
    while (vector != 0) {
        if (vector & 1u) result ^= matrix[index];
        vector >>= 1;
        index++;
    }
    return result;
}

void gf2_matrix_square(uint32_t *square, const uint32_t *matrix) {
    for (size_t n = 0; n < 32; ++n) {
        square[n] = gf2_matrix_times(matrix, matrix[n]);
    }
}

// Combine two finalized reflected IEEE CRC32 values without retaining the
// bytes of the second stream. This keeps sparse inspection streaming.
uint32_t crc32_combine_ieee(uint32_t first,
                            uint32_t second,
                            uint64_t second_length) {
    if (second_length == 0) return first;

    uint32_t odd[32] = {};
    uint32_t even[32] = {};
    odd[0] = 0xEDB88320u;
    uint32_t row = 1;
    for (size_t n = 1; n < 32; ++n) {
        odd[n] = row;
        row <<= 1;
    }

    gf2_matrix_square(even, odd);
    gf2_matrix_square(odd, even);
    do {
        gf2_matrix_square(even, odd);
        if (second_length & 1u) {
            first = gf2_matrix_times(even, first);
        }
        second_length >>= 1;
        if (second_length == 0) break;

        gf2_matrix_square(odd, even);
        if (second_length & 1u) {
            first = gf2_matrix_times(odd, first);
        }
        second_length >>= 1;
    } while (second_length != 0);

    return first ^ second;
}

bool component_is_expected_fg(ResmedFirmwareImageProfile profile,
                              const uint8_t *component) {
    if (!component) return false;

    const char *expected = component_name(profile);
    const size_t expected_length = strlen(expected);
    if (memcmp(component, expected, expected_length) != 0) return false;
    for (size_t i = expected_length; i < 16; ++i) {
        if (component[i] != 0) return false;
    }
    return true;
}

std::shared_ptr<ResmedFirmwareSegmentPlan> allocate_segment_plan() {
    try {
        return std::allocate_shared<ResmedFirmwareSegmentPlan>(
            LargeAllocator<ResmedFirmwareSegmentPlan>());
    } catch (const std::bad_alloc &) {
        return {};
    }
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

bool parse_uint_field(const char *text, size_t &offset, unsigned &value) {
    if (!text || !isdigit(static_cast<unsigned char>(text[offset]))) {
        return false;
    }

    unsigned parsed = 0;
    do {
        const unsigned digit = static_cast<unsigned>(text[offset] - '0');
        if (parsed > (UINT_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        offset++;
    } while (isdigit(static_cast<unsigned char>(text[offset])));

    value = parsed;
    return true;
}

}  // namespace

ResmedFirmwareImageProfile resmed_firmware_image_profile_for_identifier(
    const char *device_identifier) {
    if (device_identifier && strncmp(device_identifier, "SW03900.", 8) == 0) {
        return ResmedFirmwareImageProfile::AirMini;
    }
    return ResmedFirmwareImageProfile::AirSense11;
}

uint64_t resmed_firmware_profile_max_container_bytes(
    ResmedFirmwareImageProfile profile) {
    return profile == ResmedFirmwareImageProfile::AirMini
        ? AC_RESMED_AIRMINI_OTA_MAX_CONTAINER_BYTES
        : UINT64_MAX;
}

bool resmed_firmware_version_from_text(const char *text,
                                       char *out,
                                       size_t out_size) {
    if (!text || !out || out_size == 0) return false;
    out[0] = '\0';

    for (size_t i = 0; text[i]; ++i) {
        if (!isdigit(static_cast<unsigned char>(text[i]))) continue;

        int fields[4] = {};
        int consumed = 0;
        if (sscanf(text + i, "%d.%d.%d.%d%n",
                   &fields[0], &fields[1], &fields[2], &fields[3],
                   &consumed) != 4) {
            continue;
        }
        if (fields[0] <= 0 || fields[0] >= 100) continue;

        bool valid = true;
        for (size_t field = 1; field < 4; ++field) {
            if (fields[field] < 0 || fields[field] >= 100) valid = false;
        }
        if (!valid) continue;

        const int length = snprintf(out, out_size, "%d.%d.%d.%d",
                                    fields[0], fields[1], fields[2],
                                    fields[3]);
        return length > 0 && static_cast<size_t>(length) < out_size;
    }
    return false;
}

bool resmed_firmware_bootloader_version_from_text(const char *text,
                                                  char *out,
                                                  size_t out_size) {
    if (!text || !out || out_size == 0) return false;
    out[0] = '\0';

    for (size_t start = 0; text[start]; ++start) {
        if (!isdigit(static_cast<unsigned char>(text[start])) ||
            (start > 0 && isdigit(static_cast<unsigned char>(text[start - 1])))) {
            continue;
        }

        size_t offset = start;
        unsigned fields[3] = {};
        bool valid = true;
        for (size_t field = 0; field < 3; ++field) {
            if (!parse_uint_field(text, offset, fields[field])) {
                valid = false;
                break;
            }
            if (text[offset] != '.') {
                valid = false;
                break;
            }
            offset++;
        }
        if (!valid) continue;

        size_t hash_length = 0;
        while (isxdigit(static_cast<unsigned char>(text[offset + hash_length]))) {
            hash_length++;
        }
        if (hash_length < 7) continue;

        const int length = snprintf(out, out_size, "%u.%u.%u",
                                    fields[0], fields[1], fields[2]);
        return length > 0 && static_cast<size_t>(length) < out_size;
    }
    return false;
}

bool resmed_firmware_identify_fgbl(
    uint64_t image_size,
    const uint8_t *boot_id,
    size_t boot_id_size,
    char *version_out,
    size_t version_out_size) {
    if (image_size != AC_RESMED_FGBL_BYTES || !boot_id ||
        boot_id_size != AC_RESMED_FGBL_BOOT_ID_BYTES || !version_out ||
        version_out_size == 0) {
        return false;
    }

    char identifier[AC_RESMED_FGBL_BOOT_ID_BYTES + 1] = {};
    memcpy(identifier, boot_id, AC_RESMED_FGBL_BOOT_ID_BYTES);
    return resmed_firmware_bootloader_version_from_text(
        identifier, version_out, version_out_size);
}

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

bool resmed_firmware_target_range(ResmedFirmwareTarget target,
                                  uint32_t &flash_start,
                                  uint64_t &payload_size) {
    return resmed_firmware_target_range_for_profile(
        ResmedFirmwareImageProfile::AirSense11, target,
        flash_start, payload_size);
}

bool resmed_firmware_target_range_for_profile(
    ResmedFirmwareImageProfile profile,
    ResmedFirmwareTarget target,
    uint32_t &flash_start,
    uint64_t &payload_size) {
    const FirmwareTargetSpec *spec = target_spec(target);
    if (!spec) return false;

    flash_start = spec->flash_start;
    payload_size = target_payload_size(profile, *spec);
    return true;
}

bool ResmedFirmwareInspector::begin(uint64_t input_size,
                                    const char *filename,
                                    const char *device_identifier,
                                    ResmedFirmwareTarget target,
                                    ResmedFirmwareInstallTransport transport) {
    *this = ResmedFirmwareInspector();
    if (input_size == 0) return fail("empty_image");

    info_.profile = resmed_firmware_image_profile_for_identifier(
        device_identifier);
    if (info_.profile == ResmedFirmwareImageProfile::AirMini &&
        transport == ResmedFirmwareInstallTransport::Service) {
        return fail("service_unsupported_for_airmini");
    }
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
    const FirmwareTargetSpec *target =
        target_spec(requested_target_);
    if (!target) return fail("unsupported_target");
    const uint64_t target_size = target_payload_size(info_.profile, *target);

    if (info_.input_size == target_size) {
        info_.source_offset = 0;
    } else if (info_.input_size == profile_full_flash_bytes(info_.profile)) {
        info_.source_offset = target->flash_start - FlashBase;
    } else {
        return fail("raw_image_size_mismatch");
    }

    uint32_t word_2 = 0;
    uint32_t word_3 = 0;
    bool have_preset = false;
    if (resmed_firmware_version_from_text(
            device_identifier_, info_.descriptor_version,
            sizeof(info_.descriptor_version))) {
        have_preset = descriptor_preset(info_.profile,
                                        info_.descriptor_version,
                                        word_2, word_3);
    }
    if (!have_preset &&
        info_.profile == ResmedFirmwareImageProfile::AirSense11 &&
        resmed_firmware_version_from_text(
            filename_, info_.descriptor_version,
            sizeof(info_.descriptor_version))) {
        have_preset = descriptor_preset(info_.profile,
                                        info_.descriptor_version,
                                        word_2, word_3);
    }
    if (!have_preset) {
        if (transport_ == ResmedFirmwareInstallTransport::Rpc &&
            ((info_.profile == ResmedFirmwareImageProfile::AirMini &&
              (target->descriptor_word_2 || target->descriptor_word_3)) ||
             (info_.profile == ResmedFirmwareImageProfile::AirSense11 &&
              requested_target_ != ResmedFirmwareTarget::Apcx &&
              requested_target_ != ResmedFirmwareTarget::Fgcb))) {
            return fail("unsupported_descriptor_preset");
        }

        word_2 = 0;
        word_3 = 0;
        if (transport_ == ResmedFirmwareInstallTransport::Rpc &&
            info_.profile == ResmedFirmwareImageProfile::AirSense11 &&
            requested_target_ == ResmedFirmwareTarget::Apcx) {
            word_3 = latest_descriptor_preset(info_.profile).word_3;
        }
    }

    info_.kind = ResmedFirmwareImageKind::Raw;
    info_.payload_size = target_size;
    info_.service_source_offset = info_.source_offset;
    info_.service_payload_size = target_size;
    info_.prepared_size = AC_RESMED_RAW_ABC_PREFIX_BYTES +
                          target_size;
    info_.flash_start = target->flash_start;
    if (target->descriptor_word_2) info_.descriptor_word_2 = word_2;
    if (target->descriptor_word_3) info_.descriptor_word_3 = word_3;
    copy_text(info_.target, sizeof(info_.target), target->code);

    sparse_raw_ = info_.profile == ResmedFirmwareImageProfile::AirMini &&
                  requested_target_ == ResmedFirmwareTarget::Fgcb &&
                  info_.input_size == target_size;
    if (sparse_raw_) {
        sparse_plan_ = allocate_segment_plan();
        if (!sparse_plan_) return fail("segment_plan_alloc_failed");
        sparse_plan_->count = 0;
        sparse_plan_->data_size = 0;
        info_.segment_plan = sparse_plan_;
        info_.prepared_size = 0;
    }

    if (!sparse_raw_) {
        uint8_t segment[AC_RESMED_ABC_SEGMENT_BYTES] = {};
        put_le32(segment, 0, static_cast<uint32_t>(target_size));
        put_le32(segment, 4, target->flash_start);
        rest_crc_state_ = crc32_ieee_update_state(
            crc32_ieee_initial_state(), segment, sizeof(segment));
    }
    configured_ = true;
    return true;
}

bool ResmedFirmwareInspector::configure_abc_0005() {
    if (info_.input_size < AC_RESMED_ABC_0005_HEADER_BYTES) {
        return fail("abc_0005_too_short");
    }
    if (info_.profile == ResmedFirmwareImageProfile::AirMini &&
        info_.input_size >= AC_RESMED_AIRMINI_OTA_MAX_CONTAINER_BYTES) {
        return fail("airmini_container_too_large");
    }

    info_.kind = ResmedFirmwareImageKind::Abc0005;
    info_.prepared_size = info_.input_size;
    header_required_ = AC_RESMED_ABC_0005_HEADER_BYTES;
    configured_ = true;
    return true;
}

bool ResmedFirmwareInspector::configure_abc_0006() {
    if (info_.profile == ResmedFirmwareImageProfile::AirMini) {
        return fail("unsupported_abc_format");
    }
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
    if (sparse_raw_) {
        // Mini FGCB erases all twelve sectors before writing the listed
        // segments, so erased words can be omitted without changing flash.
        for (size_t i = 0; i < copy_length; ++i) {
            sparse_partial_[sparse_partial_bytes_++] =
                data[data_offset + i];
            if (sparse_partial_bytes_ != sizeof(sparse_partial_)) continue;

            if (!consume_sparse_word(sparse_word_offset_, sparse_partial_)) {
                return false;
            }
            sparse_word_offset_ += sizeof(sparse_partial_);
            sparse_partial_bytes_ = 0;
        }
        payload_received_ += copy_length;
        return true;
    }

    rest_crc_state_ = crc32_ieee_update_state(
        rest_crc_state_, data + data_offset, copy_length);
    payload_received_ += copy_length;
    return true;
}

bool ResmedFirmwareInspector::consume_sparse_word(
    uint64_t payload_offset,
    const uint8_t word[4]) {
    const bool erased = word[0] == 0xFF && word[1] == 0xFF &&
                        word[2] == 0xFF && word[3] == 0xFF;
    if (erased) {
        if (sparse_gap_bytes_ == 0) {
            sparse_gap_crc_state_ = crc32_ieee_initial_state();
        }
        sparse_gap_crc_state_ = crc32_ieee_update_state(
            sparse_gap_crc_state_, word, sizeof(sparse_partial_));
        sparse_gap_bytes_ += sizeof(sparse_partial_);

        if (sparse_current_active_) {
            if (sparse_gap_bytes_ > MiniSparseMergeGapBytes) {
                return close_sparse_segment();
            }
        } else if (sparse_plan_->count > 0) {
            ResmedFirmwareSegmentPlanEntry &previous =
                sparse_plan_->entries[sparse_plan_->count - 1];
            previous.gap_bytes = sparse_gap_bytes_;
            previous.gap_crc = crc32_ieee_finish_state(
                sparse_gap_crc_state_);
        }
        return true;
    }

    if (sparse_current_active_) {
        ResmedFirmwareSegmentPlanEntry &current =
            sparse_plan_->entries[sparse_current_segment_];
        if (sparse_gap_bytes_ > MiniSparseMergeGapBytes) {
            return fail("mini_sparse_gap_state");
        }
        if (sparse_gap_bytes_ != 0) {
            uint8_t erased_bytes[MiniSparseMergeGapBytes] = {};
            memset(erased_bytes, 0xFF, sizeof(erased_bytes));
            sparse_current_data_crc_state_ = crc32_ieee_update_state(
                sparse_current_data_crc_state_, erased_bytes,
                sparse_gap_bytes_);
            current.segment.length += sparse_gap_bytes_;
            sparse_gap_bytes_ = 0;
            sparse_gap_crc_state_ = 0;
            current.gap_bytes = 0;
            current.gap_crc = 0;
        }
        sparse_current_data_crc_state_ = crc32_ieee_update_state(
            sparse_current_data_crc_state_, word, sizeof(sparse_partial_));
        current.segment.length += sizeof(sparse_partial_);
        return true;
    }

    sparse_gap_bytes_ = 0;
    sparse_gap_crc_state_ = 0;
    if (sparse_plan_->count == AC_RESMED_MAX_ABC_SEGMENTS + 1 &&
        !merge_smallest_sparse_gap()) {
        return false;
    }

    sparse_current_segment_ = sparse_plan_->count++;
    ResmedFirmwareSegmentPlanEntry &current =
        sparse_plan_->entries[sparse_current_segment_];
    current = {};
    current.segment.flash_start = info_.flash_start +
                                  static_cast<uint32_t>(payload_offset);
    current.segment.length = sizeof(sparse_partial_);
    sparse_current_data_crc_state_ = crc32_ieee_update_state(
        crc32_ieee_initial_state(), word, sizeof(sparse_partial_));
    sparse_current_active_ = true;
    return true;
}

bool ResmedFirmwareInspector::close_sparse_segment() {
    if (!sparse_current_active_ || !sparse_plan_) return true;

    ResmedFirmwareSegmentPlanEntry &current =
        sparse_plan_->entries[sparse_current_segment_];
    current.data_crc = crc32_ieee_finish_state(
        sparse_current_data_crc_state_);
    current.gap_bytes = sparse_gap_bytes_;
    current.gap_crc = sparse_gap_bytes_ == 0
        ? 0
        : crc32_ieee_finish_state(sparse_gap_crc_state_);
    sparse_current_active_ = false;
    return true;
}

bool ResmedFirmwareInspector::merge_smallest_sparse_gap() {
    if (!sparse_plan_ || sparse_plan_->count <= AC_RESMED_MAX_ABC_SEGMENTS) {
        return true;
    }

    size_t merge_index = 0;
    for (size_t i = 1; i + 1 < sparse_plan_->count; ++i) {
        if (sparse_plan_->entries[i].gap_bytes <
            sparse_plan_->entries[merge_index].gap_bytes) {
            merge_index = i;
        }
    }

    ResmedFirmwareSegmentPlanEntry &left =
        sparse_plan_->entries[merge_index];
    const ResmedFirmwareSegmentPlanEntry right =
        sparse_plan_->entries[merge_index + 1];
    const uint32_t merged_length = left.segment.length + left.gap_bytes +
                                   right.segment.length;

    uint32_t merged_crc = left.data_crc;
    if (left.gap_bytes != 0) {
        merged_crc = crc32_combine_ieee(
            merged_crc, left.gap_crc, left.gap_bytes);
    }
    merged_crc = crc32_combine_ieee(
        merged_crc, right.data_crc, right.segment.length);
    left.segment.length = static_cast<uint32_t>(merged_length);
    left.data_crc = merged_crc;
    left.gap_bytes = right.gap_bytes;
    left.gap_crc = right.gap_crc;

    for (size_t i = merge_index + 1; i + 1 < sparse_plan_->count; ++i) {
        sparse_plan_->entries[i] = sparse_plan_->entries[i + 1];
    }
    sparse_plan_->count--;
    return true;
}

bool ResmedFirmwareInspector::finalize_sparse_segments() {
    if (!sparse_plan_) return fail("segment_plan_missing");

    // Region CRC coverage is not established here. Preserve every non-erased
    // source byte and let the device validate its own region metadata.
    if (sparse_current_active_) {
        // A trailing erased gap is intentionally omitted from the segment.
        sparse_gap_bytes_ = 0;
        sparse_gap_crc_state_ = 0;
        if (!close_sparse_segment()) return false;
    }
    if (sparse_partial_bytes_ != 0) return fail("mini_sparse_partial_word");
    if (sparse_plan_->count == 0) return fail("mini_sparse_empty");

    while (sparse_plan_->count > AC_RESMED_MAX_ABC_SEGMENTS) {
        if (!merge_smallest_sparse_gap()) return false;
    }

    uint32_t table_crc_state = crc32_ieee_initial_state();
    uint32_t data_crc = 0;
    uint64_t data_size = 0;
    bool have_data = false;
    uint8_t table_entry[AC_RESMED_ABC_SEGMENT_BYTES] = {};
    for (size_t i = 0; i < sparse_plan_->count; ++i) {
        const ResmedFirmwareSegmentPlanEntry &entry =
            sparse_plan_->entries[i];
        data_size += entry.segment.length;

        put_le32(table_entry, 0, entry.segment.length);
        put_le32(table_entry, 4, entry.segment.flash_start);
        table_crc_state = crc32_ieee_update_state(
            table_crc_state, table_entry, sizeof(table_entry));

        if (!have_data) {
            data_crc = entry.data_crc;
            have_data = true;
        } else {
            data_crc = crc32_combine_ieee(
                data_crc, entry.data_crc, entry.segment.length);
        }
    }

    const size_t prefix_size = AC_RESMED_ABC_0005_HEADER_BYTES +
        sparse_plan_->count * AC_RESMED_ABC_SEGMENT_BYTES;
    info_.prepared_size = prefix_size + data_size;
    if (info_.prepared_size >=
        resmed_firmware_profile_max_container_bytes(info_.profile)) {
        return fail("airmini_container_too_large");
    }

    sparse_plan_->data_size = data_size;
    info_.segment_plan = sparse_plan_;
    info_.rest_crc = crc32_combine_ieee(
        crc32_ieee_finish_state(table_crc_state), data_crc, data_size);
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
    if (!component_is_expected_fg(info_.profile, header_ + 0x48)) {
        return fail(info_.profile == ResmedFirmwareImageProfile::AirMini
                        ? "airmini_component_mismatch"
                        : "airsense11_component_mismatch");
    }

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
    const uint64_t target_size = target_payload_size(info_.profile, *target);
    target_flash_end_ = static_cast<uint64_t>(target->flash_start) +
                        target_size;
    target_payload_size_ = target_size;

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
    if (info_.profile == ResmedFirmwareImageProfile::AirMini &&
        ((start - info_.flash_start) % MiniSegmentAlignment != 0 ||
         length % MiniSegmentAlignment != 0)) {
        return fail("mini_segment_unaligned");
    }
    if (info_.profile == ResmedFirmwareImageProfile::AirMini &&
        segments_parsed_ != 0 && start < previous_segment_end_) {
        return fail("mini_segment_overlap");
    }
    if (segment_data_bytes_ > UINT64_MAX - length) {
        return fail("abc_segment_size_overflow");
    }

    if (segments_parsed_ == 0) {
        first_segment_start_ = start;
        first_segment_length_ = length;
    }
    previous_segment_end_ = static_cast<uint32_t>(end);
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
        if (sparse_raw_) {
            if (!finalize_sparse_segments()) return false;
            return true;
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

size_t resmed_raw_abc_prefix_size(const ResmedFirmwareImageInfo &info) {
    if (info.kind != ResmedFirmwareImageKind::Raw || info.payload_size == 0) {
        return 0;
    }
    const size_t segment_count = info.segment_plan
        ? info.segment_plan->count
        : 1;
    if (segment_count == 0 || segment_count > AC_RESMED_MAX_ABC_SEGMENTS) {
        return 0;
    }
    return AC_RESMED_ABC_0005_HEADER_BYTES +
           segment_count * AC_RESMED_ABC_SEGMENT_BYTES;
}

bool resmed_build_raw_abc_prefix(const ResmedFirmwareImageInfo &info,
                                 uint8_t *out,
                                 size_t out_size) {
    const size_t prefix_size = resmed_raw_abc_prefix_size(info);
    const size_t segment_count = info.segment_plan
        ? info.segment_plan->count
        : 1;
    const uint64_t data_size = info.segment_plan
        ? info.segment_plan->data_size
        : info.payload_size;
    if (!out || prefix_size == 0 || out_size < prefix_size ||
        !info.valid() || info.payload_size > UINT32_MAX ||
        data_size > UINT32_MAX) {
        return false;
    }
    const uint64_t rest_size =
        static_cast<uint64_t>(segment_count) * AC_RESMED_ABC_SEGMENT_BYTES +
        data_size;
    if (rest_size > UINT32_MAX ||
        info.prepared_size != prefix_size + data_size) {
        return false;
    }

    memset(out, 0, prefix_size);
    memcpy(out, "OTA!", 4);
    memcpy(out + 4, "0005", 4);
    const char *component = component_name(info.profile);
    memcpy(out + 0x48, component, strlen(component));

    uint8_t *descriptor = out + AC_RESMED_ABC_PRIMARY_BYTES;
    put_le32(descriptor, 0x00, 1);
    memcpy(descriptor + 0x04, info.target, 4);
    put_le32(descriptor, 0x08, info.descriptor_word_2);
    put_le32(descriptor, 0x0C, info.descriptor_word_3);
    put_le32(descriptor, 0x10, 0);
    put_le32(descriptor, 0x40, static_cast<uint32_t>(rest_size));
    put_le32(descriptor, 0x44, info.rest_crc);
    put_le32(descriptor, 0x48, segment_count);

    uint32_t descriptor_crc = crc32_ieee_initial_state();
    descriptor_crc = crc32_ieee_update_state(
        descriptor_crc, out, AC_RESMED_ABC_PRIMARY_BYTES);
    descriptor_crc = crc32_ieee_update_state(
        descriptor_crc, descriptor, 0x4C);
    put_le32(descriptor, 0x4C,
             crc32_ieee_finish_state(descriptor_crc));

    uint8_t *table = out + AC_RESMED_ABC_0005_HEADER_BYTES;
    for (size_t i = 0; i < segment_count; ++i) {
        const ResmedFirmwareSegment implicit_segment = {
            static_cast<uint32_t>(info.payload_size), info.flash_start};
        const ResmedFirmwareSegment &segment = info.segment_plan
            ? info.segment_plan->entries[i].segment
            : implicit_segment;
        if (segment.length == 0 || segment.length % MiniSegmentAlignment != 0 ||
            segment.flash_start % MiniSegmentAlignment != 0) {
            return false;
        }
        put_le32(table, 0, segment.length);
        put_le32(table, 4, segment.flash_start);
        table += AC_RESMED_ABC_SEGMENT_BYTES;
    }
    return true;
}

bool resmed_build_raw_abc_prefix(
    const ResmedFirmwareImageInfo &info,
    uint8_t out[AC_RESMED_RAW_ABC_PREFIX_BYTES]) {
    return resmed_build_raw_abc_prefix(
        info, out, AC_RESMED_RAW_ABC_PREFIX_BYTES);
}

}  // namespace aircannect
