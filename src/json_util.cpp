#include "json_util.h"

#include <math.h>
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
void json_add_string_impl(Out &out, const char *key, const char *value, bool comma) {
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
void append_json_float_impl(Out &out, float value) {
    if (!isfinite(value)) {
        out += "null";
        return;
    }
    const bool negative = value < 0.0f;
    const float abs_value = negative ? -value : value;
    const unsigned long scaled =
        static_cast<unsigned long>(abs_value * 1000.0f + 0.5f);
    const unsigned long whole = scaled / 1000UL;
    const unsigned long frac = scaled % 1000UL;

    char buf[24];
    if (negative && scaled != 0) out += '-';
    snprintf(buf, sizeof(buf), "%lu", whole);
    out += buf;
    if (!frac) return;

    char frac_buf[3];
    frac_buf[0] = static_cast<char>('0' + (frac / 100UL) % 10UL);
    frac_buf[1] = static_cast<char>('0' + (frac / 10UL) % 10UL);
    frac_buf[2] = static_cast<char>('0' + frac % 10UL);
    size_t digits = 3;
    while (digits > 0 && frac_buf[digits - 1] == '0') --digits;
    out += '.';
    for (size_t i = 0; i < digits; ++i) out += frac_buf[i];
}

template <typename Out>
void json_add_float_impl(Out &out, const char *key, float value, bool comma) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";
    append_json_float_impl(out, value);
}

template <typename Out>
void json_add_uint64_impl(Out &out, const char *key, uint64_t value, bool comma) {
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

#if AIRCANNECT_JSON_UTIL_HAS_ARDUINO
void append_json_escaped(String &out, const char *value) {
    append_json_escaped_impl(out, value, value ? strlen(value) : 0);
}

void append_json_escaped(String &out, const char *value, size_t len) {
    append_json_escaped_impl(out, value, len);
}

void append_json_float(String &out, float value) {
    append_json_float_impl(out, value);
}

void json_add_string(String &out, const char *key, const char *value, bool comma) {
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

void json_add_uint64(String &out, const char *key, uint64_t value, bool comma) {
    json_add_uint64_impl(out, key, value, comma);
}
#endif

void append_json_escaped(LargeTextBuffer &out, const char *value) {
    append_json_escaped_impl(out, value, value ? strlen(value) : 0);
}

void append_json_escaped(LargeTextBuffer &out, const char *value, size_t len) {
    append_json_escaped_impl(out, value, len);
}

void append_json_float(LargeTextBuffer &out, float value) {
    append_json_float_impl(out, value);
}

void json_add_string(LargeTextBuffer &out, const char *key, const char *value, bool comma) {
    json_add_string_impl(out, key, value, comma);
}

void json_add_string_view(LargeTextBuffer &out, const char *key, std::string_view value, bool comma) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":\"";
    append_json_escaped_impl(out, value.data(), value.size());
    out += '"';
}

void json_add_bool(LargeTextBuffer &out, const char *key, bool value, bool comma) {
    json_add_bool_impl(out, key, value, comma);
}

void json_add_int(LargeTextBuffer &out, const char *key, long value, bool comma) {
    json_add_int_impl(out, key, value, comma);
}

void json_add_float(LargeTextBuffer &out, const char *key, float value, bool comma) {
    json_add_float_impl(out, key, value, comma);
}

void json_add_uint64(LargeTextBuffer &out, const char *key, uint64_t value, bool comma) {
    json_add_uint64_impl(out, key, value, comma);
}

}  // namespace aircannect
