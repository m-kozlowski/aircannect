#include "data_id_csv.h"

#include <string.h>

namespace aircannect {
namespace {

bool token_valid(const char *token, size_t token_len) {
    if (!token || token_len == 0) return false;

    for (size_t i = 0; i < token_len; ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        if (c < 0x20 || c == 0x7f || c == ',' || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

void trim_token(const char *&token, size_t &token_len) {
    while (token_len > 0 && (*token == ' ' || *token == '\t')) {
        token++;
        token_len--;
    }
    while (token_len > 0 &&
           (token[token_len - 1] == ' ' || token[token_len - 1] == '\t')) {
        token_len--;
    }
}

bool next_token(const char *&pos, const char *&token, size_t &token_len) {
    while (pos && *pos) {
        while (*pos == ',' || *pos == ' ' || *pos == '\t') pos++;
        if (!*pos) break;

        token = pos;
        while (*pos && *pos != ',') pos++;
        token_len = static_cast<size_t>(pos - token);
        trim_token(token, token_len);

        if (*pos == ',') pos++;
        if (token_len > 0) {
            return true;
        }
    }

    token = nullptr;
    token_len = 0;
    return false;
}

}  // namespace

bool data_id_csv_contains(const std::string &csv,
                          const char *token,
                          size_t token_len) {
    if (!token || token_len == 0) return false;

    const char *pos = csv.c_str();
    const char *candidate = nullptr;
    size_t candidate_len = 0;

    while (next_token(pos, candidate, candidate_len)) {
        if (candidate_len == token_len &&
            memcmp(candidate, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}

bool data_id_csv_covers(const std::string &available_csv,
                        const char *requested_csv) {
    const char *pos = requested_csv;
    const char *token = nullptr;
    size_t token_len = 0;
    bool saw_token = false;

    while (next_token(pos, token, token_len)) {
        saw_token = true;
        if (!data_id_csv_contains(available_csv, token, token_len)) {
            return false;
        }
    }
    return saw_token;
}

bool data_id_csv_add(std::string &csv,
                     size_t &item_count,
                     const char *token,
                     size_t token_len,
                     const DataIdCsvLimits &limits) {
    trim_token(token, token_len);
    if (!token_valid(token, token_len)) return false;
    if (data_id_csv_contains(csv, token, token_len)) return true;

    if (item_count >= limits.max_items ||
        token_len > limits.max_token_bytes) {
        return false;
    }

    const size_t separator_bytes = csv.empty() ? 0 : 1;
    if (csv.size() > limits.max_csv_bytes ||
        token_len > limits.max_csv_bytes - csv.size() ||
        separator_bytes > limits.max_csv_bytes - csv.size() - token_len) {
        return false;
    }

    if (separator_bytes) csv.push_back(',');
    csv.append(token, token_len);
    item_count++;
    return true;
}

bool data_id_csv_merge(std::string &csv,
                       size_t &item_count,
                       const char *input_csv,
                       const DataIdCsvLimits &limits) {
    const char *pos = input_csv;
    const char *token = nullptr;
    size_t token_len = 0;

    while (next_token(pos, token, token_len)) {
        if (!data_id_csv_add(csv, item_count, token, token_len, limits)) {
            return false;
        }
    }
    return true;
}

bool data_id_csv_append_json_array(std::string &out, const char *csv) {
    const size_t original_size = out.size();
    out.push_back('[');

    const char *pos = csv;
    const char *token = nullptr;
    size_t token_len = 0;
    bool first = true;

    while (next_token(pos, token, token_len)) {
        if (!token_valid(token, token_len)) {
            out.resize(original_size);
            return false;
        }

        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out.append(token, token_len);
        out.push_back('"');
    }

    out.push_back(']');
    return true;
}

}  // namespace aircannect
