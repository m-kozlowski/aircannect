#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace aircannect {

struct JsonCursor {
    const char *pos = nullptr;
    const char *end = nullptr;
    char *error = nullptr;
    size_t error_len = 0;

    JsonCursor(const char *payload,
               size_t payload_len,
               char *error_buf = nullptr,
               size_t error_buf_len = 0)
        : error(error_buf), error_len(error_buf_len) {
        const char *base = payload ? payload : "";
        pos = base;
        end = base + (payload ? payload_len : 0);
    }

    explicit JsonCursor(const std::string &payload,
                        char *error_buf = nullptr,
                        size_t error_buf_len = 0)
        : pos(payload.c_str()),
          end(payload.c_str() + payload.size()),
          error(error_buf),
          error_len(error_buf_len) {}

    void fail(const char *message) {
        if (!error || error_len == 0) return;
        snprintf(error, error_len, "%s", message ? message : "");
    }

    void skip_ws() {
        while (pos < end && isspace(static_cast<unsigned char>(*pos))) pos++;
    }

    bool consume(char c) {
        skip_ws();
        if (pos >= end || *pos != c) return false;
        pos++;
        return true;
    }

    bool parse_string(char *out, size_t out_size) {
        skip_ws();
        if (pos >= end || *pos != '"') {
            fail("expected string");
            return false;
        }
        pos++;
        size_t written = 0;
        while (pos < end) {
            char c = *pos++;
            if (c == '"') {
                if (out && out_size) {
                    out[written < out_size ? written : out_size - 1] = 0;
                }
                return true;
            }
            if (!decode_string_char(c)) return false;
            if (out && out_size && written + 1 < out_size) {
                out[written++] = c;
            }
        }
        fail("unterminated string");
        return false;
    }

    bool parse_string(std::string &out) {
        skip_ws();
        if (pos >= end || *pos != '"') {
            fail("expected string");
            return false;
        }
        pos++;
        out.clear();
        while (pos < end) {
            char c = *pos++;
            if (c == '"') return true;
            if (!decode_string_char(c)) return false;
            out += c;
        }
        fail("unterminated string");
        return false;
    }

    bool parse_uint(uint32_t &value) {
        skip_ws();
        if (pos >= end || !isdigit(static_cast<unsigned char>(*pos))) {
            fail("expected unsigned integer");
            return false;
        }
        uint32_t out = 0;
        while (pos < end && isdigit(static_cast<unsigned char>(*pos))) {
            out = out * 10u + static_cast<uint32_t>(*pos - '0');
            pos++;
        }
        value = out;
        return true;
    }

    bool parse_float(float &value) {
        skip_ws();
        const char *start = pos;
        while (pos < end) {
            const char c = *pos;
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
                c == '.' || c == 'e' || c == 'E') {
                pos++;
                continue;
            }
            break;
        }
        const size_t len = static_cast<size_t>(pos - start);
        if (len == 0 || len >= 64) {
            fail("expected number");
            pos = start;
            return false;
        }
        char buf[64];
        memcpy(buf, start, len);
        buf[len] = 0;
        char *next = nullptr;
        value = strtof(buf, &next);
        if (next == buf || *next != 0) {
            fail("expected number");
            pos = start;
            return false;
        }
        return true;
    }

    bool consume_literal(const char *literal) {
        skip_ws();
        const size_t len = strlen(literal);
        if (static_cast<size_t>(end - pos) < len ||
            strncmp(pos, literal, len) != 0) {
            return false;
        }
        pos += len;
        return true;
    }

    bool skip_value() {
        skip_ws();
        if (pos >= end) {
            fail("expected value");
            return false;
        }
        if (*pos == '"') {
            return parse_string(nullptr, 0);
        }
        if (*pos == '{') {
            pos++;
            skip_ws();
            if (pos < end && *pos == '}') {
                pos++;
                return true;
            }
            while (pos < end) {
                if (!parse_string(nullptr, 0)) return false;
                if (!consume(':')) {
                    fail("expected object colon");
                    return false;
                }
                if (!skip_value()) return false;
                skip_ws();
                if (pos < end && *pos == ',') {
                    pos++;
                    continue;
                }
                if (pos < end && *pos == '}') {
                    pos++;
                    return true;
                }
                fail("expected object separator");
                return false;
            }
            fail("unterminated object");
            return false;
        }
        if (*pos == '[') {
            pos++;
            skip_ws();
            if (pos < end && *pos == ']') {
                pos++;
                return true;
            }
            while (pos < end) {
                if (!skip_value()) return false;
                skip_ws();
                if (pos < end && *pos == ',') {
                    pos++;
                    continue;
                }
                if (pos < end && *pos == ']') {
                    pos++;
                    return true;
                }
                fail("expected array separator");
                return false;
            }
            fail("unterminated array");
            return false;
        }
        if (consume_literal("null") || consume_literal("true") ||
            consume_literal("false")) {
            return true;
        }
        float ignored = 0.0f;
        return parse_float(ignored);
    }

private:
    bool decode_string_char(char &c) {
        if (c != '\\') return true;
        if (pos >= end) {
            fail("unterminated escape");
            return false;
        }
        c = *pos++;
        switch (c) {
            case '"':
            case '\\':
            case '/':
                return true;
            case 'b': c = '\b'; return true;
            case 'f': c = '\f'; return true;
            case 'n': c = '\n'; return true;
            case 'r': c = '\r'; return true;
            case 't': c = '\t'; return true;
            case 'u':
                for (uint8_t i = 0; i < 4 && pos < end; ++i) pos++;
                c = '?';
                return true;
            default:
                fail("bad escape");
                return false;
        }
    }
};

}  // namespace aircannect
