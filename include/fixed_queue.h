#pragma once

#include <new>
#include <stddef.h>
#include <utility>

namespace aircannect {

template <typename T, size_t N>
class FixedQueue {
public:
    bool push(const T &value) {
        if (full()) {
            dropped_++;
            return false;
        }
        items_[tail_] = value;
        tail_ = (tail_ + 1) % N;
        count_++;
        return true;
    }

    bool push(T &&value) {
        if (full()) {
            dropped_++;
            return false;
        }
        items_[tail_] = std::move(value);
        tail_ = (tail_ + 1) % N;
        count_++;
        return true;
    }

    bool push_front(const T &value) {
        if (full()) {
            dropped_++;
            return false;
        }
        head_ = (head_ + N - 1) % N;
        items_[head_] = value;
        count_++;
        return true;
    }

    bool push_front(T &&value) {
        if (full()) {
            dropped_++;
            return false;
        }
        head_ = (head_ + N - 1) % N;
        items_[head_] = std::move(value);
        count_++;
        return true;
    }

    bool pop(T &value) {
        if (empty()) return false;
        value = std::move(items_[head_]);
        reset_item(items_[head_]);
        head_ = (head_ + 1) % N;
        count_--;
        return true;
    }

    void clear() {
        for (size_t i = 0; i < N; ++i) reset_item(items_[i]);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == N; }
    size_t count() const { return count_; }
    size_t capacity() const { return N; }
    size_t free() const { return N - count_; }
    unsigned long dropped() const { return dropped_; }
    void reset_dropped() { dropped_ = 0; }

private:
    static void reset_item(T &item) {
        item.~T();
        new (&item) T{};
    }

    T items_[N] = {};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    unsigned long dropped_ = 0;
};

}  // namespace aircannect
