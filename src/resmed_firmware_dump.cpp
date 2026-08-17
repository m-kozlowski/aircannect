#include "resmed_firmware_dump.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "resmed_firmware_catalog.h"
#include "resmed_firmware_image.h"

namespace aircannect {
namespace {

bool extract_family(const char *product_name, char *out, size_t out_size) {
    if (!product_name || !out || out_size < 3) return false;

    size_t written = 0;
    bool saw_letter = false;
    bool saw_digit = false;
    for (const char *cursor = product_name; *cursor; ++cursor) {
        const unsigned char value = static_cast<unsigned char>(*cursor);
        if (isalpha(value) && !saw_digit) {
            if (written + 1 >= out_size) return false;
            out[written++] = static_cast<char>(tolower(value));
            saw_letter = true;
            continue;
        }
        if (isdigit(value) && saw_letter) {
            if (written + 1 >= out_size) return false;
            out[written++] = static_cast<char>(value);
            saw_digit = true;
            continue;
        }
        if (saw_digit) break;
    }

    out[written] = '\0';
    return saw_letter && saw_digit;
}

bool format_path(char *out,
                 size_t out_size,
                 const char *format,
                 const char *first,
                 const char *second,
                 unsigned variant_id) {
    const int length = snprintf(out, out_size, format, first, second,
                                variant_id);
    return length > 0 && static_cast<size_t>(length) < out_size;
}

}  // namespace

bool resmed_firmware_dump_identity(
    const char *product_name,
    const char *software_identifier,
    const char *bootloader_identifier,
    int32_t variant_id,
    ResmedFirmwareDumpIdentity &out) {
    out = {};
    if (variant_id < 0 || variant_id > 99 ||
        !extract_family(product_name, out.family, sizeof(out.family)) ||
        !resmed_firmware_version_from_text(
            software_identifier, out.version, sizeof(out.version)) ||
        !resmed_firmware_bootloader_version_from_text(
            bootloader_identifier, out.bootloader_version,
            sizeof(out.bootloader_version))) {
        return false;
    }

    out.variant_id = static_cast<uint8_t>(variant_id);
    if (!format_path(out.filename, sizeof(out.filename),
                     "dump-%s-%s_vid%02u.bin", out.family, out.version,
                     out.variant_id)) {
        return false;
    }

    const int output_length = snprintf(
        out.output_path, sizeof(out.output_path), "%s/%s",
        AC_RESMED_FIRMWARE_REPOSITORY_PATH, out.filename);
    if (output_length <= 0 ||
        static_cast<size_t>(output_length) >= sizeof(out.output_path)) {
        return false;
    }

    char bootloader_directory[AC_STORAGE_PATH_MAX] = {};
    if (!resmed_firmware_patched_bootloader_path(
            out.bootloader_version, bootloader_directory,
            sizeof(bootloader_directory), out.patched_bootloader_path,
            sizeof(out.patched_bootloader_path))) {
        return false;
    }
    return true;
}

}  // namespace aircannect
