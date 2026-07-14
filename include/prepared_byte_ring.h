#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class PreparedByteReadState : uint8_t {
    Data,
    Retry,
    End,
};

struct PreparedByteRead {
    PreparedByteReadState state = PreparedByteReadState::End;
    size_t bytes = 0;
};

class PreparedByteRing {
public:
    void bind(uint8_t *storage, size_t capacity);
    void clear();

    size_t readable() const { return size_; }
    size_t writable() const { return capacity_ - size_; }
    size_t capacity() const { return capacity_; }

    uint8_t *write_span(size_t &length);
    bool commit_write(size_t length);
    size_t read(uint8_t *destination, size_t max_length);

private:
    uint8_t *storage_ = nullptr;
    size_t capacity_ = 0;
    size_t head_ = 0;
    size_t size_ = 0;
    size_t offered_write_ = 0;
};

}  // namespace aircannect
