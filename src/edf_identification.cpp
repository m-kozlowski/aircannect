#include "edf_identification.h"

#include <ArduinoJson.h>

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

bool copy_identification_member(JsonObject target,
                               JsonObjectConst source,
                               const char *name) {
    JsonVariantConst value = source[name];
    if (value.isNull()) return false;
    target[name] = value;
    return true;
}

}  // namespace

bool edf_build_identification_json(RpcPayloadView get_response,
                                   std::string &json_out,
                                   ResmedDeviceModel model) {
    json_out.clear();

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, get_response.data() ? get_response.data() : "",
        get_response.size());
    if (err) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) return false;

    const JsonObjectConst profiles =
        result["IdentificationProfiles"].as<JsonObjectConst>();
    if (!profiles.isNull()) {
        std::string result_json;
        serializeJson(result, result_json);
        if (result_json.empty()) return false;

        json_out.reserve(result_json.size() + 20);
        json_out = "{\"FlowGenerator\":";
        json_out += result_json;
        json_out += '}';
    } else {
        if (model != ResmedDeviceModel::AirMini) return false;

        JsonDocument normalized;
        JsonObject flow = normalized["FlowGenerator"].to<JsonObject>();
        JsonObject normalized_profiles =
            flow["IdentificationProfiles"].to<JsonObject>();
        JsonObject product = normalized_profiles["Product"].to<JsonObject>();
        JsonObject software = normalized_profiles["Software"].to<JsonObject>();
        bool copied = false;
        copied = copy_identification_member(product, result, "ProductName") ||
                 copied;
        copied = copy_identification_member(product, result, "SerialNumber") ||
                 copied;
        copied = copy_identification_member(product, result, "ProductCode") ||
                 copied;
        copied = copy_identification_member(
                     software, result, "ApplicationIdentifier") ||
                 copied;
        copied = copy_identification_member(
                     software, result, "BootloaderIdentifier") ||
                 copied;
        copied = copy_identification_member(
                     software, result, "PlatformIdentifier") ||
                 copied;
        copied = copy_identification_member(
                     software, result, "VariantIdentifier") ||
                 copied;
        if (!copied) return false;
        serializeJson(normalized, json_out);
        if (json_out.empty()) return false;
    }

    strip_identification_whitespace(json_out);
    return true;
}

void edf_identification_crc32_le(uint32_t crc, uint8_t out[4]) {
    if (!out) return;
    out[0] = static_cast<uint8_t>(crc & 0xffu);
    out[1] = static_cast<uint8_t>((crc >> 8) & 0xffu);
    out[2] = static_cast<uint8_t>((crc >> 16) & 0xffu);
    out[3] = static_cast<uint8_t>((crc >> 24) & 0xffu);
}

}  // namespace aircannect
