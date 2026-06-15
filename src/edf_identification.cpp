#include "edf_identification.h"

#include <ArduinoJson.h>

#include "crc32.h"

namespace aircannect {
namespace {

bool identification_ignored_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void strip_identification_whitespace(std::string &json) {
    size_t out = 0;
    for (size_t in = 0; in < json.size(); ++in) {
        if (!identification_ignored_whitespace(json[in])) {
            json[out++] = json[in];
        }
    }
    json.resize(out);
}

}  // namespace

bool edf_build_identification_json(const std::string &get_response,
                                   std::string &json_out) {
    json_out.clear();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, get_response);
    if (err) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull() ||
        result["IdentificationProfiles"].as<JsonObjectConst>().isNull()) {
        return false;
    }

    std::string result_json;
    serializeJson(result, result_json);
    if (result_json.empty()) return false;

    json_out.reserve(result_json.size() + 20);
    json_out = "{\"FlowGenerator\":";
    json_out += result_json;
    json_out += '}';
    strip_identification_whitespace(json_out);
    return true;
}

uint32_t edf_identification_crc32(const std::string &json) {
    return crc32_ieee(reinterpret_cast<const uint8_t *>(json.data()),
                      json.size());
}

void edf_identification_crc32_le(uint32_t crc, uint8_t out[4]) {
    if (!out) return;
    out[0] = static_cast<uint8_t>(crc & 0xffu);
    out[1] = static_cast<uint8_t>((crc >> 8) & 0xffu);
    out[2] = static_cast<uint8_t>((crc >> 16) & 0xffu);
    out[3] = static_cast<uint8_t>((crc >> 24) & 0xffu);
}

}  // namespace aircannect
