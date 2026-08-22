#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class LargeByteBuffer {
public:
    static std::unique_ptr<LargeByteBuffer> allocate(size_t size);
    static std::shared_ptr<const LargeByteBuffer> copy_and_freeze(
        const void *data, size_t size);
    static std::shared_ptr<const LargeByteBuffer> freeze(
        std::unique_ptr<LargeByteBuffer> buffer);
    static std::shared_ptr<const LargeByteBuffer> slice(
        const std::shared_ptr<const LargeByteBuffer> &parent,
        size_t offset,
        size_t size);

    ~LargeByteBuffer();
    LargeByteBuffer(const LargeByteBuffer &) = delete;
    LargeByteBuffer &operator=(const LargeByteBuffer &) = delete;

    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }
    bool grow(size_t size);
    bool truncate(size_t size);

private:
    LargeByteBuffer(uint8_t *data, size_t size) : data_(data), size_(size) {}
    LargeByteBuffer(const std::shared_ptr<const LargeByteBuffer> &parent,
                    size_t offset,
                    size_t size) :
        data_(const_cast<uint8_t *>(parent->data()) + offset),
        size_(size),
        parent_(parent) {}

    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    std::shared_ptr<const LargeByteBuffer> parent_;
};

}  // namespace aircannect
