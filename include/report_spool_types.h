#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace aircannect {

static constexpr size_t AC_REPORT_MAX_PAYLOAD_BYTES = 2 * 1024 * 1024;

class ReportSpoolBuffer {
public:
    ReportSpoolBuffer() = default;
    ~ReportSpoolBuffer();

    ReportSpoolBuffer(const ReportSpoolBuffer &) = delete;
    ReportSpoolBuffer &operator=(const ReportSpoolBuffer &) = delete;

    void clear();
    void move_from(ReportSpoolBuffer &other);
    bool reserve_capacity(size_t capacity);
    bool append(const uint8_t *data, size_t len);
    uint8_t *append_uninitialized(size_t len, size_t &offset);
    void truncate(size_t size);
    void set_max_size(size_t max_size);
    const uint8_t *data() const { return data_; }
    uint8_t *mutable_data() { return data_; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    size_t max_size() const { return max_size_; }

private:
    bool reserve(size_t capacity);

    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    size_t max_size_ = AC_REPORT_MAX_PAYLOAD_BYTES;
};

struct ReportSpoolResult {
    std::string spool_type;
    std::string from_dt;
    std::string terminal_status;
    std::string spool_hash;
    std::string computed_sha256;
    bool sha_ok = false;
    bool truncated = false;
    bool complete = false;
    uint16_t rounds = 0;
    uint32_t fragments = 0;
    uint32_t bytes = 0;
    ReportSpoolBuffer payload;

    void clear();
    void move_from(ReportSpoolResult &other);
};

}  // namespace aircannect
