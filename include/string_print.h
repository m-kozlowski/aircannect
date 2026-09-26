#pragma once

#include <Arduino.h>
#include <algorithm>
#include <cstring>

namespace aircannect {

class StringPrint : public Print {
public:
    explicit StringPrint(size_t max_len = 4096,
                         const char *line_ending = "\n")
        : max_len_(max_len),
          line_ending_(line_ending),
          max_capacity_(max_len + TRUNCATION_MESSAGE_LENGTH +
                        (line_ending ? 2 * std::strlen(line_ending) : 0)) {
        const size_t initial_capacity = std::min(max_len_, INITIAL_CAPACITY);
        if (initial_capacity &&
            text_.reserve(static_cast<unsigned int>(initial_capacity))) {
            reserved_capacity_ = initial_capacity;
        }
    }

    size_t write(uint8_t c) override {
        if (text_.length() < max_len_) {
            const char value = static_cast<char>(c);
            append(&value, 1);
        }
        return 1;
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        if (!buffer || size == 0) return 0;
        const size_t room = max_len_ > text_.length()
                                ? max_len_ - text_.length()
                                : 0;
        const size_t copy = std::min(room, size);
        if (copy) append(reinterpret_cast<const char *>(buffer), copy);
        if (copy < size && !truncated_) {
            append_line_ending();
            append("[CONSOLE] output truncated",
                   TRUNCATION_MESSAGE_LENGTH);
            append_line_ending();
            truncated_ = true;
        }
        return size;
    }

    const String &text() const { return text_; }

private:
    static constexpr size_t INITIAL_CAPACITY = 128;
    static constexpr size_t TRUNCATION_MESSAGE_LENGTH =
        sizeof("[CONSOLE] output truncated") - 1;

    void reserve_geometrically(size_t required) {
        if (required <= reserved_capacity_) return;

        size_t capacity = std::min(
            reserved_capacity_ ? reserved_capacity_ : INITIAL_CAPACITY,
            max_capacity_);
        while (capacity < required) {
            capacity = std::min(capacity * 2, max_capacity_);
        }

        if (text_.reserve(static_cast<unsigned int>(capacity))) {
            reserved_capacity_ = capacity;
        }
    }

    void append(const char *data, size_t length) {
        const size_t current = text_.length();
        reserve_geometrically(current + length);
        (void)text_.concat(data, static_cast<unsigned int>(length));
    }

    void append_line_ending() {
        if (line_ending_ && *line_ending_) {
            append(line_ending_, std::strlen(line_ending_));
        }
    }

    String text_;
    size_t max_len_;
    const char *line_ending_;
    size_t max_capacity_;
    size_t reserved_capacity_ = 0;
    bool truncated_ = false;
};

}  // namespace aircannect
