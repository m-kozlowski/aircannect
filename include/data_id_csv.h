#pragma once

#include <stddef.h>
#include <string>

namespace aircannect {

struct DataIdCsvLimits {
    size_t max_items = 0;
    size_t max_token_bytes = 0;
    size_t max_csv_bytes = 0;
};

bool data_id_csv_contains(const std::string &csv,
                          const char *token,
                          size_t token_len);
bool data_id_csv_covers(const std::string &available_csv,
                        const char *requested_csv);
bool data_id_csv_add(std::string &csv,
                     size_t &item_count,
                     const char *token,
                     size_t token_len,
                     const DataIdCsvLimits &limits);
bool data_id_csv_merge(std::string &csv,
                       size_t &item_count,
                       const char *input_csv,
                       const DataIdCsvLimits &limits);
bool data_id_csv_append_json_array(std::string &out, const char *csv);

}  // namespace aircannect
