#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace aircannect {

class LargeTextBuffer {
public:
    LargeTextBuffer() = default;
    ~LargeTextBuffer();

    LargeTextBuffer(const LargeTextBuffer &) = delete;
    LargeTextBuffer &operator=(const LargeTextBuffer &) = delete;

    bool reserve(size_t capacity);
    void clear();

    size_t length() const { return length_; }
    size_t capacity() const { return capacity_; }
    bool overflowed() const { return overflowed_; }
    const char *c_str() const { return data_ ? data_ : ""; }

    LargeTextBuffer &operator=(const char *text);
    LargeTextBuffer &operator+=(const char *text);
    LargeTextBuffer &operator+=(char c);
    LargeTextBuffer &operator+=(const String &text);

    bool append(const char *text);
    bool append(const char *text, size_t len);

private:
    bool ensure_capacity(size_t needed);

    char *data_ = nullptr;
    size_t length_ = 0;
    size_t capacity_ = 0;
    bool overflowed_ = false;
};

}  // namespace aircannect
