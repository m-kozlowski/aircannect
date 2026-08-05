#pragma once

#include <cstring>
#include <memory>
#include <stddef.h>
#include <utility>

#include "large_byte_buffer.h"

namespace aircannect {

using RpcPayloadRef = std::shared_ptr<const LargeByteBuffer>;

struct RpcPayloadView {
    constexpr RpcPayloadView() = default;

    constexpr RpcPayloadView(const char *data, size_t size)
        : data_(data), size_(data ? size : 0) {}

    const char *data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

private:
    const char *data_ = nullptr;
    size_t size_ = 0;
};

inline RpcPayloadRef copy_rpc_payload(const void *data, size_t size) {
    if (size == 0) return {};
    if (!data) return {};

    std::unique_ptr<LargeByteBuffer> bytes = LargeByteBuffer::allocate(size);
    if (!bytes) return {};

    std::memcpy(bytes->data(), data, size);
    return LargeByteBuffer::freeze(std::move(bytes));
}

inline RpcPayloadView rpc_payload_view(const RpcPayloadRef &payload) {
    if (!payload || payload->size() == 0) return {};

    return RpcPayloadView(
        reinterpret_cast<const char *>(payload->data()), payload->size());
}

}  // namespace aircannect
