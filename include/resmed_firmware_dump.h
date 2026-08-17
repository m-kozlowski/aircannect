#pragma once

#include <stdint.h>

#include "storage_path.h"

namespace aircannect {

struct ResmedFirmwareDumpIdentity {
    char family[24] = {};
    char version[16] = {};
    char bootloader_version[16] = {};
    char filename[96] = {};
    char output_path[AC_STORAGE_PATH_MAX] = {};
    char patched_bootloader_path[AC_STORAGE_PATH_MAX] = {};
    uint8_t variant_id = 0;
};

bool resmed_firmware_dump_identity(const char *product_name,
                                   const char *software_identifier,
                                   const char *bootloader_identifier,
                                   int32_t variant_id,
                                   ResmedFirmwareDumpIdentity &out);

}  // namespace aircannect
