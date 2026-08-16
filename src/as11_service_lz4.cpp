#include "as11_service_lz4.h"

#include <algorithm>

#include <lz4.h>

#include "as11_service_protocol.h"

namespace aircannect {

size_t as11_service_lz4_state_bytes() {
    const int bytes = LZ4_sizeofState();
    return bytes > 0 ? static_cast<size_t>(bytes) : 0;
}

bool as11_service_compress_lz4_block(const uint8_t *source,
                                     size_t source_size,
                                     uint8_t *destination,
                                     size_t destination_capacity,
                                     void *state,
                                     size_t state_size,
                                     size_t &compressed_size) {
    compressed_size = 0;
    const size_t required_state = as11_service_lz4_state_bytes();
    if (!source || source_size == 0 ||
        source_size > AS11_SERVICE_LZ4_RAW_MAX_BYTES || !destination ||
        !state || required_state == 0 || state_size < required_state) {
        return false;
    }

    const size_t useful_capacity = std::min({
        destination_capacity,
        AS11_SERVICE_WRITE_LZ4_DATA_MAX_BYTES,
        source_size - 1,
    });
    if (useful_capacity == 0) return true;

    const int encoded = LZ4_compress_fast_extState(
        state,
        reinterpret_cast<const char *>(source),
        reinterpret_cast<char *>(destination),
        static_cast<int>(source_size),
        static_cast<int>(useful_capacity),
        1);
    if (encoded > 0) compressed_size = static_cast<size_t>(encoded);
    return true;
}

}  // namespace aircannect
