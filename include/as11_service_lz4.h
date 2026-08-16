#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

size_t as11_service_lz4_state_bytes();

// compressed_size remains zero when the block would not become smaller.
bool as11_service_compress_lz4_block(const uint8_t *source,
                                     size_t source_size,
                                     uint8_t *destination,
                                     size_t destination_capacity,
                                     void *state,
                                     size_t state_size,
                                     size_t &compressed_size);

}  // namespace aircannect
