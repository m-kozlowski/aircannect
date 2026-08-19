#include "string_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

namespace aircannect {

void copy_cstr(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

#if AIRCANNECT_STRING_UTIL_HAS_ARDUINO
void trim_inplace(String &value) {
    value.trim();
}

void to_lower_inplace(String &value) {
    value.toLowerCase();
}
#endif

std::string lower_compact_copy(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(static_cast<char>(
            tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

#if AIRCANNECT_STRING_UTIL_HAS_ARDUINO
bool parse_bool_yesno(const String &text, bool &value) {
    String normalized = text;
    trim_inplace(normalized);
    to_lower_inplace(normalized);
    if (normalized == "on" || normalized == "yes" ||
        normalized == "true" || normalized == "1" ||
        normalized == "enabled" || normalized == "enable") {
        value = true;
        return true;
    }
    if (normalized == "off" || normalized == "no" ||
        normalized == "false" || normalized == "0" ||
        normalized == "disabled" || normalized == "disable") {
        value = false;
        return true;
    }
    return false;
}
#endif

bool parse_bool_yesno(const std::string &text, bool &value) {
    size_t start = 0;
    while (start < text.size() &&
           isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    size_t end = text.size();
    while (end > start && isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }

    std::string normalized;
    normalized.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        normalized.push_back(static_cast<char>(
            tolower(static_cast<unsigned char>(text[i]))));
    }

    if (normalized == "on" || normalized == "yes" ||
        normalized == "true" || normalized == "1" ||
        normalized == "enabled" || normalized == "enable") {
        value = true;
        return true;
    }
    if (normalized == "off" || normalized == "no" ||
        normalized == "false" || normalized == "0" ||
        normalized == "disabled" || normalized == "disable") {
        value = false;
        return true;
    }
    return false;
}

bool parse_uint64_decimal(const char *text, uint64_t &value) {
    if (!text || !*text) return false;

    uint64_t parsed = 0;
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;

        const uint8_t digit = static_cast<uint8_t>(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }

    value = parsed;
    return true;
}

bool parse_uint32_decimal(const char *text, uint32_t &value) {
    uint64_t parsed = 0;
    if (!parse_uint64_decimal(text, parsed) || parsed > UINT32_MAX) {
        return false;
    }

    value = static_cast<uint32_t>(parsed);
    return true;
}

#if AIRCANNECT_STRING_UTIL_HAS_ARDUINO
bool parse_port(String text, uint16_t &port) {
    trim_inplace(text);
    if (!text.length()) return false;
    const char *start = text.c_str();
    char *end = nullptr;
    unsigned long parsed = strtoul(start, &end, 0);
    if (end == start || *end != '\0' || parsed == 0 || parsed > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_index(String text, size_t count, size_t &index) {
    trim_inplace(text);
    if (!text.length()) return false;
    const char *start = text.c_str();
    char *end = nullptr;
    unsigned long parsed = strtoul(start, &end, 0);
    if (end == start || *end != '\0' || parsed >= count) return false;
    index = static_cast<size_t>(parsed);
    return true;
}
#endif

}  // namespace aircannect
