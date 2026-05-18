#include "management_console_utils.h"

#include <ctype.h>

#include "string_util.h"

namespace aircannect {

std::string to_std(const String &s) {
    return std::string(s.c_str());
}

bool parse_console_arg(const String &text, int &pos, String &out) {
    out = "";
    while (pos < static_cast<int>(text.length()) &&
           isspace(static_cast<unsigned char>(text[pos]))) {
        pos++;
    }
    if (pos >= static_cast<int>(text.length())) return false;

    char quote = 0;
    if (text[pos] == '"' || text[pos] == '\'') {
        quote = text[pos++];
    }

    while (pos < static_cast<int>(text.length())) {
        char c = text[pos++];
        if (quote) {
            if (c == quote) return true;
            if (c == '\\' && pos < static_cast<int>(text.length())) {
                out += text[pos++];
            } else {
                out += c;
            }
        } else if (isspace(static_cast<unsigned char>(c))) {
            return true;
        } else {
            out += c;
        }
    }

    return quote == 0;
}

bool parse_on_off(String value, bool &enabled) {
    return parse_bool_yesno(value, enabled);
}

const char *on_off_text(bool enabled) {
    return enabled ? "on" : "off";
}

bool parse_uint16_arg(String text, uint16_t &value) {
    return parse_port(text, value);
}

bool parse_index_arg(String text, size_t count, size_t &index) {
    return parse_index(text, count, index);
}

void print_unknown_command(Print &out,
                           const char *category,
                           const char *usage) {
    out.print("[");
    out.print(category);
    out.print("] unknown command. Try: ");
    out.println(usage);
}

}  // namespace aircannect
