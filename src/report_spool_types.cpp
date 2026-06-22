#include "report_spool_types.h"

#include <stdlib.h>
#include <string.h>
#include <utility>

#ifdef ARDUINO
#include "debug_log.h"
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void *alloc_report_buffer(size_t size) {
#ifdef ARDUINO
    return Memory::alloc_large(size);
#else
    return malloc(size);
#endif
}

void free_report_buffer(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

void log_report_alloc_failure(size_t capacity, size_t current) {
#ifdef ARDUINO
    Log::logf(CAT_REPORT,
              LOG_ERROR,
              "spool buffer allocation failed bytes=%u current=%u\n",
              static_cast<unsigned>(capacity),
              static_cast<unsigned>(current));
#else
    (void)capacity;
    (void)current;
#endif
}

}  // namespace

ReportSpoolBuffer::~ReportSpoolBuffer() {
    free_report_buffer(data_);
}

void ReportSpoolBuffer::clear() {
    free_report_buffer(data_);
    data_ = nullptr;
    size_ = 0;
    capacity_ = 0;
}

void ReportSpoolBuffer::move_from(ReportSpoolBuffer &other) {
    if (this == &other) return;
    clear();
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    max_size_ = other.max_size_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    other.max_size_ = AC_REPORT_MAX_PAYLOAD_BYTES;
}

bool ReportSpoolBuffer::reserve(size_t capacity) {
    if (capacity <= capacity_) return true;
    uint8_t *next = static_cast<uint8_t *>(alloc_report_buffer(capacity));
    if (!next) {
        log_report_alloc_failure(capacity, capacity_);
        return false;
    }
    if (data_ && size_) memcpy(next, data_, size_);
    free_report_buffer(data_);
    data_ = next;
    capacity_ = capacity;
    return true;
}

bool ReportSpoolBuffer::reserve_capacity(size_t capacity) {
    if (capacity > max_size_) return false;
    return reserve(capacity);
}

bool ReportSpoolBuffer::append(const uint8_t *data, size_t len) {
    if (!data || len == 0) return true;
    size_t offset = 0;
    uint8_t *tail = append_uninitialized(len, offset);
    if (!tail) return false;
    memcpy(tail, data, len);
    return true;
}

uint8_t *ReportSpoolBuffer::append_uninitialized(size_t len, size_t &offset) {
    offset = size_;
    if (len == 0) return data_ ? data_ + size_ : nullptr;
    if (len > max_size_ - size_) return nullptr;
    const size_t needed = size_ + len;
    size_t target = capacity_ ? capacity_ : 4096;
    if (target < needed) {
        if (needed <= 128 * 1024) {
            while (target < needed) {
                if (target > max_size_ / 2) {
                    target = needed;
                    break;
                }
                target *= 2;
            }
        } else {
            const size_t step = 64 * 1024;
            target = ((needed + step - 1) / step) * step;
        }
    }
    if (target > max_size_) {
        target = max_size_;
    }
    if (!reserve(target)) return nullptr;
    uint8_t *tail = data_ + size_;
    size_ += len;
    return tail;
}

void ReportSpoolBuffer::truncate(size_t size) {
    if (size < size_) size_ = size;
}

void ReportSpoolBuffer::set_max_size(size_t max_size) {
    max_size_ = max_size ? max_size : AC_REPORT_MAX_PAYLOAD_BYTES;
    if (size_ > max_size_) size_ = max_size_;
}

void ReportSpoolResult::clear() {
    spool_type.clear();
    from_dt.clear();
    terminal_status.clear();
    spool_hash.clear();
    computed_sha256.clear();
    sha_ok = false;
    truncated = false;
    complete = false;
    rounds = 0;
    fragments = 0;
    bytes = 0;
    payload.clear();
}

void ReportSpoolResult::move_from(ReportSpoolResult &other) {
    if (this == &other) return;
    clear();
    spool_type = std::move(other.spool_type);
    from_dt = std::move(other.from_dt);
    terminal_status = std::move(other.terminal_status);
    spool_hash = std::move(other.spool_hash);
    computed_sha256 = std::move(other.computed_sha256);
    sha_ok = other.sha_ok;
    truncated = other.truncated;
    complete = other.complete;
    rounds = other.rounds;
    fragments = other.fragments;
    bytes = other.bytes;
    payload.move_from(other.payload);
    other.clear();
}

}  // namespace aircannect
