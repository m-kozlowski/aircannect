#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace aircannect {

std::string to_std(const String &s);
bool parse_console_arg(const String &text, int &pos, String &out);
bool parse_on_off(String value, bool &enabled);
const char *on_off_text(bool enabled);
bool parse_uint16_arg(String text, uint16_t &value);
bool parse_index_arg(String text, size_t count, size_t &index);

void print_unknown_command(Print &out,
                           const char *category,
                           const char *usage);

}  // namespace aircannect
