#include "airmini_ncp_rpc.h"

#include <cstring>

#include "hex_util.h"
#include "large_json_allocator.h"
#include "little_endian.h"
#include "utc_time.h"

namespace aircannect {
namespace {

bool valid_datetime(std::string_view text) {
    if (text.size() != 24 || text[23] != 'Z') return false;

    int64_t epoch_ms = 0;
    const std::string terminated(text);
    return parse_utc_iso8601_ms(terminated.c_str(), epoch_ms);
}

bool decode_digest(JsonVariantConst value, uint8_t *out) {
    const char *text = value.as<const char *>();
    if (!text || strlen(text) != 64) return false;

    size_t length = 0;
    return hex_decode(text, 64, out, 32, length);
}

bool encode_upgrade_payload(uint8_t command, JsonVariantConst params,
                            uint8_t *out, size_t &length) {
    using namespace LittleEndian;
    length = 0;

    if (command == 0x15) {
        if (!params["upgradeFileSize"].is<uint32_t>()) return false;
        const uint32_t size = params["upgradeFileSize"].as<uint32_t>();
        if (size == 0 || size >= 0x100000) return false;

        put_le32(out, size);
        length = 4;
        return true;
    }

    if (command == 0x16) {
        if (!params["fileOffset"].is<uint32_t>() ||
            params["encoding"] != "AsciiHex") return false;

        const uint32_t offset = params["fileOffset"].as<uint32_t>();
        const char *data = params["data"].as<const char *>();
        size_t bytes = 0;
        if (!data || !hex_decode(data, strlen(data), out + 6, 500, bytes) ||
            bytes == 0 || offset > UINT32_MAX - bytes) return false;

        put_le32(out, offset);
        put_le16(out + 4, static_cast<uint16_t>(bytes));
        length = 6 + bytes;
        return true;
    }

    if (command != 0x17 && command != 0x18 && command != 0x42) return false;
    if (!decode_digest(params["upgradeFileHash"], out + 2)) return false;

    put_le16(out, 32);
    length = 34;
    if (command == 0x18) {
        // Omitting this byte on Mini would reset settings by default.
        if (!params["resetSettingsToDefault"].is<bool>()) return false;

        out[length++] = params["resetSettingsToDefault"].as<bool>() ? 1 : 0;
    } else if (command == 0x42) {
        if (!decode_digest(params["authentication"], out + 36)) return false;

        put_le16(out + 34, 32);
        length = 68;
    }
    return true;
}

}  // namespace

uint8_t airmini_ncp_command(std::string_view method) {
    if (method == "GetDateTime") return 0x04;
    if (method == "SetDateTime") return 0x05;
    if (method == "InitiateUpgrade") return 0x15;
    if (method == "UpgradeDataBlock") return 0x16;
    if (method == "CheckUpgradeFile") return 0x17;
    if (method == "ApplyUpgrade") return 0x18;
    if (method == "ApplyAuthenticatedUpgrade") return 0x42;
    return 0;
}

bool encode_airmini_ncp_rpc(uint8_t command, uint8_t tag,
                            const std::string &params, std::string &record) {
    if (command == 0x04) return encode_ncp_record(command, tag, {}, record);
    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, params)) return false;

    if (command == 0x05) {
        const char *datetime = document["dateTime"].as<const char *>();
        if (!datetime || !valid_datetime(datetime)) return false;

        return encode_ncp_record(command, tag, datetime, record);
    }

    uint8_t payload[506];
    size_t length = 0;
    if (!encode_upgrade_payload(command, document.as<JsonVariantConst>(),
                                 payload, length)) return false;

    return encode_ncp_record(command, tag,
        {reinterpret_cast<const char *>(payload), length}, record);
}

RpcPayloadRef decode_airmini_ncp_rpc(const NcpRecordView &record,
                                     uint32_t request_id) {
    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    document["jsonrpc"] = "2.0";
    document["id"] = request_id;

    if (record.command == 0xfd) {
        NcpErrorView error;
        if (!decode_ncp_error(record.payload, error)) return {};

        document["error"]["code"] = error.code;
        document["error"]["message"] = std::string(error.message);
        if (!error.detail.empty()) {
            std::string hex(error.detail.size() * 2 + 1, '\0');
            hex_encode(reinterpret_cast<const uint8_t *>(error.detail.data()),
                       error.detail.size(), &hex[0], hex.size(), HexCase::Lower);
            hex.resize(hex.size() - 1);
            document["error"]["data"]["detailHex"] = hex;
        }
    } else if (record.command == 0x84 || record.command == 0x85) {
        if (record.payload.size() != 26 ||
            LittleEndian::get_le16(reinterpret_cast<const uint8_t *>(
                record.payload.data())) != 24 ||
            !valid_datetime(record.payload.substr(2))) {
            return {};
        }

        document["result"]["dateTime"] = std::string(record.payload.substr(2));
    } else if (record.command == 0x95 || record.command == 0x98 ||
               record.command == 0xc2) {
        if (record.payload.size() != 2) return {};

        const uint16_t value = LittleEndian::get_le16(
            reinterpret_cast<const uint8_t *>(record.payload.data()));
        document["result"][record.command == 0x95
            ? "xferBlockSize" : "applyResult"] = value;
    } else if (record.command == 0x96 || record.command == 0x97) {
        if (!record.payload.empty()) return {};

        document["result"] = true;
    } else {
        return {};
    }

    if (document.overflowed()) return {};

    std::string json;
    serializeJson(document, json);
    return copy_rpc_payload(json.data(), json.size());
}

}  // namespace aircannect
