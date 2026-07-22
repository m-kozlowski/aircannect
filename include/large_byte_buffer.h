#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class LargeByteBuffer {
public:
    static std::unique_ptr<LargeByteBuffer> allocate(size_t size);
    static std::shared_ptr<const LargeByteBuffer> freeze(
        std::unique_ptr<LargeByteBuffer> buffer);

    ~LargeByteBuffer();
    LargeByteBuffer(const LargeByteBuffer &) = delete;
    LargeByteBuffer &operator=(const LargeByteBuffer &) = delete;

    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }

private:
    LargeByteBuffer(uint8_t *data, size_t size) : data_(data), size_(size) {}

    uint8_t *data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace aircannect
