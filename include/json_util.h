#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string_view>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#define AIRCANNECT_JSON_UTIL_HAS_ARDUINO 1
#else
#define AIRCANNECT_JSON_UTIL_HAS_ARDUINO 0
#endif

namespace aircannect {

class LargeTextBuffer;

#if AIRCANNECT_JSON_UTIL_HAS_ARDUINO
void json_add_string(String &out, const char *key, const char *value, bool comma = true);
void json_add_bool(String &out, const char *key, bool value, bool comma = true);
void json_add_int(String &out, const char *key, long value, bool comma = true);
#endif

void append_json_escaped(LargeTextBuffer &out, const char *value, size_t len);
void append_json_float(LargeTextBuffer &out, float value);
void json_add_string(LargeTextBuffer &out, const char *key, const char *value, bool comma = true);
void json_add_string_view(LargeTextBuffer &out, const char *key, std::string_view value, bool comma = true);
void json_add_bool(LargeTextBuffer &out, const char *key, bool value, bool comma = true);
void json_add_int(LargeTextBuffer &out, const char *key, long value, bool comma = true);
void json_add_float(LargeTextBuffer &out, const char *key, float value, bool comma = true);
void json_add_uint64(LargeTextBuffer &out, const char *key, uint64_t value, bool comma = true);

}  // namespace aircannect
