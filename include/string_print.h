#pragma once

#include <Arduino.h>
#include <algorithm>

namespace aircannect {

class StringPrint : public Print {
public:
    explicit StringPrint(size_t max_len = 4096,
                         const char *line_ending = "\n")
        : max_len_(max_len), line_ending_(line_ending) {
        text_.reserve(max_len_);
    }

    size_t write(uint8_t c) override {
        if (text_.length() < max_len_) text_ += static_cast<char>(c);
        return 1;
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        if (!buffer || size == 0) return 0;
        const size_t room = max_len_ > text_.length()
                                ? max_len_ - text_.length()
                                : 0;
        const size_t copy = std::min(room, size);
        for (size_t i = 0; i < copy; ++i) {
            text_ += static_cast<char>(buffer[i]);
        }
        if (copy < size && !truncated_) {
            text_ += line_ending_;
            text_ += "[CONSOLE] output truncated";
            text_ += line_ending_;
            truncated_ = true;
        }
        return size;
    }

    const String &text() const { return text_; }

private:
    String text_;
    size_t max_len_;
    const char *line_ending_;
    bool truncated_ = false;
};

}  // namespace aircannect
