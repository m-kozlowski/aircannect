#include "json_util.h"

#if AIRCANNECT_JSON_UTIL_HAS_ARDUINO

#include <stdio.h>
#include <string.h>

#include "large_text_buffer.h"

namespace aircannect {
namespace {

template <typename Out>
void append_json_escaped_impl(Out &out, const char *value, size_t len) {
    if (!value) return;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(*value++);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
}

template <typename Out>
void json_add_string_impl(Out &out, const char *key, const char *value,
                          bool comma) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":\"";
    append_json_escaped_impl(out, value, value ? strlen(value) : 0);
    out += '"';
}

template <typename Out>
void json_add_bool_impl(Out &out, const char *key, bool value, bool comma) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += value ? "true" : "false";
}

template <typename Out>
void json_add_int_impl(Out &out, const char *key, long value, bool comma) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", value);
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += buf;
}

template <typename Out>
void json_add_float_impl(Out &out, const char *key, float value, bool comma) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(value));
    char *end = buf + strlen(buf);
    while (end > buf && end[-1] == '0') *--end = 0;
    if (end > buf && end[-1] == '.') *--end = 0;
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += buf;
}

template <typename Out>
void json_add_uint64_impl(Out &out, const char *key, uint64_t value,
                          bool comma) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += buf;
}

}  // namespace

void append_json_escaped(String &out, const char *value) {
    append_json_escaped_impl(out, value, value ? strlen(value) : 0);
}

void append_json_escaped(String &out, const char *value, size_t len) {
    append_json_escaped_impl(out, value, len);
}

void json_add_string(String &out, const char *key, const char *value,
                     bool comma) {
    json_add_string_impl(out, key, value, comma);
}

void json_add_bool(String &out, const char *key, bool value, bool comma) {
    json_add_bool_impl(out, key, value, comma);
}

void json_add_int(String &out, const char *key, long value, bool comma) {
    json_add_int_impl(out, key, value, comma);
}

void json_add_float(String &out, const char *key, float value, bool comma) {
    json_add_float_impl(out, key, value, comma);
}

void json_add_uint64(String &out, const char *key, uint64_t value,
                     bool comma) {
    json_add_uint64_impl(out, key, value, comma);
}

void append_json_escaped(LargeTextBuffer &out, const char *value) {
    append_json_escaped_impl(out, value, value ? strlen(value) : 0);
}

void append_json_escaped(LargeTextBuffer &out, const char *value,
                         size_t len) {
    append_json_escaped_impl(out, value, len);
}

void json_add_string(LargeTextBuffer &out, const char *key, const char *value,
                     bool comma) {
    json_add_string_impl(out, key, value, comma);
}

void json_add_string_view(LargeTextBuffer &out,
                          const char *key,
                          std::string_view value,
                          bool comma) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":\"";
    append_json_escaped_impl(out, value.data(), value.size());
    out += '"';
}

void json_add_bool(LargeTextBuffer &out, const char *key, bool value,
                   bool comma) {
    json_add_bool_impl(out, key, value, comma);
}

void json_add_int(LargeTextBuffer &out, const char *key, long value,
                  bool comma) {
    json_add_int_impl(out, key, value, comma);
}

void json_add_float(LargeTextBuffer &out, const char *key, float value,
                    bool comma) {
    json_add_float_impl(out, key, value, comma);
}

void json_add_uint64(LargeTextBuffer &out, const char *key, uint64_t value,
                     bool comma) {
    json_add_uint64_impl(out, key, value, comma);
}

}  // namespace aircannect

#endif
