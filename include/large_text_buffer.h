#pragma once

#include <stddef.h>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#define AIRCANNECT_LARGE_TEXT_BUFFER_HAS_ARDUINO 1
#else
#define AIRCANNECT_LARGE_TEXT_BUFFER_HAS_ARDUINO 0
#endif

namespace aircannect {

class LargeTextBuffer {
public:
    LargeTextBuffer() = default;
    ~LargeTextBuffer();

    LargeTextBuffer(const LargeTextBuffer &) = delete;
    LargeTextBuffer &operator=(const LargeTextBuffer &) = delete;

    bool reserve(size_t capacity);
    void clear();
    void swap(LargeTextBuffer &other);

    size_t length() const { return length_; }
    size_t capacity() const { return capacity_; }
    bool overflowed() const { return overflowed_; }
    const char *c_str() const { return data_ ? data_ : ""; }

    LargeTextBuffer &operator=(const char *text);
    LargeTextBuffer &operator+=(const char *text);
    LargeTextBuffer &operator+=(char c);
#if AIRCANNECT_LARGE_TEXT_BUFFER_HAS_ARDUINO
    LargeTextBuffer &operator+=(const String &text);
#endif

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
