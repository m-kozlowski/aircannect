#pragma once

#include <stdint.h>
#include <string>

namespace aircannect {

static constexpr const char *AC_EDF_IDENTIFICATION_JSON_PATH =
    "/Identification.json";
static constexpr const char *AC_EDF_IDENTIFICATION_CRC_PATH =
    "/Identification.crc";

bool edf_build_identification_json(const std::string &get_response,
                                   std::string &json_out);
uint32_t edf_identification_crc32(const std::string &json);
void edf_identification_crc32_le(uint32_t crc, uint8_t out[4]);

}  // namespace aircannect
