#include "as11_rpc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "data_id_csv.h"
#include "json_cursor.h"

namespace aircannect {
namespace {

bool seek_top_member(JsonCursor &json, const char *member) {
    if (!member || !json.consume('{')) return false;

    json.skip_ws();
    if (json.pos < json.end && *json.pos == '}') return false;

    while (json.pos < json.end) {
        char key[64] = {};
        if (!json.parse_string(key, sizeof(key))) return false;
        if (!json.consume(':')) return false;

        if (strcmp(key, member) == 0) return true;
        if (!json.skip_value()) return false;

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') return false;
        return false;
    }
    return false;
}

bool parse_rpc_envelope(JsonCursor &json, RpcEnvelope &envelope) {
    envelope = {};
    if (!json.consume('{')) return false;

    json.skip_ws();
    if (json.pos < json.end && *json.pos == '}') return true;

    while (json.pos < json.end) {
        char key[64] = {};
        if (!json.parse_string(key, sizeof(key))) return false;
        if (!json.consume(':')) return false;

        if (strcmp(key, "id") == 0) {
            if (!json.parse_uint(envelope.id)) return false;
            envelope.kind = RpcPayloadKind::Response;
            return true;
        }

        if (strcmp(key, "method") == 0) {
            if (!json.parse_string(envelope.method,
                                   sizeof(envelope.method))) {
                return false;
            }
            envelope.kind = RpcPayloadKind::Notification;
            return true;
        }

        if (!json.skip_value()) return false;

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') return true;
        return false;
    }
    return false;
}

}  // namespace

bool RpcEnvelope::method_is(const char *expected) const {
    return expected && kind == RpcPayloadKind::Notification &&
           strcmp(method, expected) == 0;
}

const char *rpc_version_for_method(const std::string &method) {
    if (method == "GetVersion" || method == "EnterMaskFit") return "2.0";
    if (method == "SetDateTime" || method == "ApplyUpgrade" ||
        method == "GenerateAuthCode") {
        return "1.1";
    }
    return "1.0";
}

std::string json_escape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04X",
                             static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string build_rpc_request(const std::string &method,
                              const std::string &params_json,
                              uint32_t id) {
    std::string req;
    req.reserve(method.size() + params_json.size() + 64);
    req += "{\"jsonrpc\":\"";
    req += rpc_version_for_method(method);
    req += "\",\"method\":\"";
    req += json_escape(method);
    req += "\",\"id\":";
    req += std::to_string(id);
    if (!params_json.empty()) {
        req += ",\"params\":";
        req += params_json;
    }
    req += "}";
    return req;
}

std::string build_get_params(const std::string &names) {
    std::string out = "[";
    size_t pos = 0;
    bool first = true;
    while (pos < names.size()) {
        while (pos < names.size() &&
               isspace(static_cast<unsigned char>(names[pos]))) {
            pos++;
        }
        if (pos >= names.size()) break;
        const size_t start = pos;
        while (pos < names.size() &&
               !isspace(static_cast<unsigned char>(names[pos]))) {
            pos++;
        }
        if (!first) out += ",";
        out += "\"";
        out += json_escape(names.substr(start, pos - start));
        out += "\"";
        first = false;
    }
    out += "]";
    return out;
}

std::string build_set_datetime_params(const std::string &utc_datetime) {
    std::string out;
    out.reserve(utc_datetime.size() + 20);
    out += "{\"dateTime\":\"";
    out += json_escape(utc_datetime);
    out += "\"}";
    return out;
}

void normalize_stream_intervals(uint32_t &sample_ms, uint32_t &report_ms) {
    if (sample_ms < 10) sample_ms = 10;
    if (sample_ms > 65000) sample_ms = 65000;
    sample_ms = (sample_ms / 10) * 10;
    if (sample_ms == 0) sample_ms = 10;

    if (report_ms == 0) report_ms = sample_ms * 5;
    if (report_ms < sample_ms) report_ms = sample_ms;
    if (report_ms > sample_ms * 5) report_ms = sample_ms * 5;
    if (report_ms > 300000) report_ms = 300000;
    report_ms = (report_ms / 10) * 10;
    if (report_ms == 0) report_ms = sample_ms;
}

std::string build_stream_params(const std::string &ids_csv,
                                uint32_t sample_ms,
                                uint32_t report_ms) {
    normalize_stream_intervals(sample_ms, report_ms);

    std::string params;
    params.reserve(ids_csv.size() + 96);
    params = "{\"dataIds\":";
    if (!data_id_csv_append_json_array(params, ids_csv.c_str())) return {};
    params += ",\"sampleIntervalMs\":";
    params += std::to_string(sample_ms);
    params += ",\"reportIntervalMs\":";
    params += std::to_string(report_ms);
    params += "}";
    return params;
}

bool json_member_present(const char *json, size_t len, const char *member) {
    JsonCursor cursor(json, len);
    return seek_top_member(cursor, member);
}

bool json_member_present(const std::string &json, const char *member) {
    return json_member_present(json.data(), json.size(), member);
}

bool json_extract_id(const char *json, size_t len, uint32_t &id) {
    return json_extract_uint_member(json, len, "id", id);
}

bool json_extract_id(const std::string &json, uint32_t &id) {
    return json_extract_id(json.data(), json.size(), id);
}

bool json_extract_uint_member(const char *json,
                              size_t len,
                              const char *member,
                              uint32_t &value) {
    JsonCursor cursor(json, len);
    return seek_top_member(cursor, member) && cursor.parse_uint(value);
}

bool json_extract_uint_member(const std::string &json,
                              const char *member,
                              uint32_t &value) {
    return json_extract_uint_member(json.data(), json.size(), member, value);
}

bool inspect_rpc_envelope(const char *json,
                          size_t len,
                          RpcEnvelope &envelope) {
    const char *payload = json ? json : "";
    const size_t payload_len = json ? len : 0;
    JsonCursor cursor(payload, payload_len);
    return parse_rpc_envelope(cursor, envelope);
}

}  // namespace aircannect
