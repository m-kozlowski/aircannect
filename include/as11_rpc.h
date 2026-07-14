#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace aircannect {

enum class RpcPayloadKind {
    Response,
    Notification,
    Unknown,
};

struct RpcEnvelope {
    static constexpr size_t MethodCapacity = 48;

    RpcPayloadKind kind = RpcPayloadKind::Unknown;
    uint32_t id = 0;
    char method[MethodCapacity] = {};

    bool method_is(const char *expected) const;
};

const char *rpc_version_for_method(const std::string &method);
std::string json_escape(const std::string &text);
std::string build_rpc_request(const std::string &method,
                              const std::string &params_json,
                              uint32_t id);
std::string build_get_params(const std::string &names);
std::string build_set_datetime_params(const std::string &utc_datetime);
void normalize_stream_intervals(uint32_t &sample_ms, uint32_t &report_ms);
std::string build_stream_params(const std::string &ids_csv,
                                uint32_t sample_ms,
                                uint32_t report_ms);

// Lightweight response inspection
bool inspect_rpc_envelope(const char *json, size_t len, RpcEnvelope &envelope);
bool inspect_rpc_envelope(const std::string &json, RpcEnvelope &envelope);
RpcPayloadKind classify_rpc_payload(const char *json, size_t len);
RpcPayloadKind classify_rpc_payload(const std::string &json);
bool json_has_id(const char *json, size_t len, uint32_t id);
bool json_has_id(const std::string &json, uint32_t id);
bool json_extract_id(const char *json, size_t len, uint32_t &id);
bool json_extract_id(const std::string &json, uint32_t &id);
bool json_method_is(const char *json, size_t len, const char *method);
bool json_method_is(const std::string &json, const char *method);
bool json_extract_uint_member(const char *json,
                              size_t len,
                              const char *member,
                              uint32_t &value);
bool json_extract_uint_member(const std::string &json,
                              const char *member,
                              uint32_t &value);
bool json_extract_string_member(const char *json,
                                size_t len,
                                const char *member,
                                std::string &value);
bool json_extract_string_member(const std::string &json,
                                const char *member,
                                std::string &value);
bool json_member_present(const char *json, size_t len, const char *member);
bool json_member_present(const std::string &json, const char *member);

}  // namespace aircannect
