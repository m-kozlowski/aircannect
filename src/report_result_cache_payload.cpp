#include "report_result_cache_payload.h"

#include "report_plot_format.h"

namespace aircannect {

bool report_result_json_cache_payload_valid(const uint8_t *data, size_t size) {
    return data && size >= 16 && data[0] == '{' && data[size - 1] == '}';
}

bool report_result_plot_cache_payload_valid(const uint8_t *data, size_t size) {
    if (!data || size < 8) return false;

    const uint32_t magic = static_cast<uint32_t>(data[0]) |
                           (static_cast<uint32_t>(data[1]) << 8) |
                           (static_cast<uint32_t>(data[2]) << 16) |
                           (static_cast<uint32_t>(data[3]) << 24);
    const uint16_t version = static_cast<uint16_t>(data[4]) |
                             (static_cast<uint16_t>(data[5]) << 8);

    return magic == PLOT_BIN_MAGIC && version == PLOT_BIN_VERSION;
}

}  // namespace aircannect
