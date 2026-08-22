#include "large_byte_buffer.h"

#include <new>
#include <string.h>

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

std::shared_ptr<const LargeByteBuffer> LargeByteBuffer::copy_and_freeze(
    const void *data, size_t size) {
    if (!data || size == 0) return {};

    std::unique_ptr<LargeByteBuffer> buffer = allocate(size);
    if (!buffer) return {};

    memcpy(buffer->data(), data, size);
    return freeze(std::move(buffer));
}

std::shared_ptr<const LargeByteBuffer> LargeByteBuffer::freeze(
    std::unique_ptr<LargeByteBuffer> buffer) {
    return std::shared_ptr<const LargeByteBuffer>(buffer.release());
}

bool LargeByteBuffer::grow(size_t size) {
    if (size <= size_) return true;

    uint8_t *next = static_cast<uint8_t *>(
        Memory::realloc_large(data_, size, false));
    if (!next) return false;

    data_ = next;
    size_ = size;
    return true;
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
