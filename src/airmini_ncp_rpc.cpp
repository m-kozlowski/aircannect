#include "airmini_ncp_rpc.h"

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

}  // namespace

uint8_t airmini_ncp_command(std::string_view method) {
    if (method == "GetDateTime") return 0x04;
    if (method == "SetDateTime") return 0x05;
    return 0;
}

bool encode_airmini_ncp_rpc(uint8_t command, uint8_t tag,
                            const std::string &params, std::string &record) {
    if (command == 0x04) return encode_ncp_record(command, tag, {}, record);
    if (command != 0x05) return false;

    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, params)) return false;

    const char *datetime = document["dateTime"].as<const char *>();
    if (!datetime || !valid_datetime(datetime)) return false;
    return encode_ncp_record(command, tag, datetime, record);
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
    } else {
        return {};
    }

    if (document.overflowed()) return {};

    std::string json;
    serializeJson(document, json);
    return copy_rpc_payload(json.data(), json.size());
}

}  // namespace aircannect
