#include "large_byte_buffer.h"

#include <new>
#include <stdlib.h>

#if __has_include(<Arduino.h>)
#include "memory_manager.h"
#define AIRCANNECT_LARGE_BYTE_BUFFER_HAS_ARDUINO 1
#else
#define AIRCANNECT_LARGE_BYTE_BUFFER_HAS_ARDUINO 0
#endif

namespace aircannect {
namespace {

void *allocate_bytes(size_t size) {
#if AIRCANNECT_LARGE_BYTE_BUFFER_HAS_ARDUINO
    return Memory::alloc_large(size, false);
#else
    return ::malloc(size);
#endif
}

void free_bytes(void *data) {
#if AIRCANNECT_LARGE_BYTE_BUFFER_HAS_ARDUINO
    Memory::free(data);
#else
    ::free(data);
#endif
}

}  // namespace

std::unique_ptr<LargeByteBuffer> LargeByteBuffer::allocate(size_t size) {
    if (size == 0) return {};

    uint8_t *data = static_cast<uint8_t *>(allocate_bytes(size));
    if (!data) return {};

    LargeByteBuffer *buffer = new (std::nothrow) LargeByteBuffer(data, size);
    if (!buffer) {
        free_bytes(data);
        return {};
    }
    return std::unique_ptr<LargeByteBuffer>(buffer);
}

std::shared_ptr<const LargeByteBuffer> LargeByteBuffer::freeze(
    std::unique_ptr<LargeByteBuffer> buffer) {
    return std::shared_ptr<const LargeByteBuffer>(buffer.release());
}

LargeByteBuffer::~LargeByteBuffer() {
    free_bytes(data_);
}

}  // namespace aircannect
