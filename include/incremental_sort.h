#pragma once

#include <algorithm>
#include <stddef.h>

namespace aircannect {

// Sort in place without making one poll process the entire history.
class IncrementalSort {
public:
    template <typename T, typename Compare>
    bool poll(T *values, size_t count, Compare compare, size_t budget = 32) {
        if (count < 2) return true;

        while (budget-- > 0) {
            if (inserted_ < count) {
                ++inserted_;
                std::push_heap(values, values + inserted_, compare);
                remaining_ = inserted_;
            } else if (remaining_ > 1) {
                std::pop_heap(values, values + remaining_, compare);
                --remaining_;
            } else {
                return true;
            }
        }

        return inserted_ == count && remaining_ <= 1;
    }

    void reset() { inserted_ = remaining_ = 0; }

private:
    size_t inserted_ = 0;
    size_t remaining_ = 0;
};

}  // namespace aircannect
