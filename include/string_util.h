#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#define AIRCANNECT_STRING_UTIL_HAS_ARDUINO 1
#else
#define AIRCANNECT_STRING_UTIL_HAS_ARDUINO 0
#endif

namespace aircannect {

void copy_cstr(char *dst, size_t size, const char *src);

#if AIRCANNECT_STRING_UTIL_HAS_ARDUINO
void trim_inplace(String &value);
void to_lower_inplace(String &value);
bool parse_bool_yesno(const String &text, bool &value);
bool parse_port(String text, uint16_t &port);
bool parse_index(String text, size_t count, size_t &index);
#endif

std::string lower_compact_copy(const std::string &value);
bool parse_bool_yesno(const std::string &text, bool &value);

}  // namespace aircannect
