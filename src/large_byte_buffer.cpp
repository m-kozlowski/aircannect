#include "large_byte_buffer.h"

#include <new>

#include "memory_manager.h"

namespace aircannect {

std::unique_ptr<LargeByteBuffer> LargeByteBuffer::allocate(size_t size) {
    if (size == 0) return {};

    uint8_t *data = static_cast<uint8_t *>(
        Memory::alloc_large(size, false));
    if (!data) return {};

    LargeByteBuffer *buffer = new (std::nothrow) LargeByteBuffer(data, size);
    if (!buffer) {
        Memory::free(data);
        return {};
    }
    return std::unique_ptr<LargeByteBuffer>(buffer);
}

std::shared_ptr<const LargeByteBuffer> LargeByteBuffer::freeze(
    std::unique_ptr<LargeByteBuffer> buffer) {
    return std::shared_ptr<const LargeByteBuffer>(buffer.release());
}

bool LargeByteBuffer::truncate(size_t size) {
    if (size == 0 || size > size_) return false;

    size_ = size;
    return true;
}

LargeByteBuffer::~LargeByteBuffer() {
    Memory::free(data_);
}

}  // namespace aircannect
