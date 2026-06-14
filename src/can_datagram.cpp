#include "can_datagram.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void *alloc_reassembly_bytes(size_t bytes) {
#ifdef ARDUINO
    return Memory::alloc_large(bytes);
#else
    return malloc(bytes);
#endif
}

void free_reassembly_bytes(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

size_t clamp_initial_reserve(size_t reserve) {
    if (reserve < AC_DG_INITIAL_RESERVE_BYTES) {
        return AC_DG_INITIAL_RESERVE_BYTES;
    }
    if (reserve > AC_DG_MAX_PAYLOAD_BYTES) {
        return AC_DG_MAX_PAYLOAD_BYTES;
    }
    return reserve;
}

}  // namespace

static constexpr uint8_t DG_MIDDLE = 0x00;
static constexpr uint8_t DG_START = 0x01;
static constexpr uint8_t DG_END = 0x02;
static constexpr uint8_t DG_SINGLE = 0x03;
static constexpr uint8_t DG_FLAG_MASK = 0x03;

uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

std::string hex_bytes(const uint8_t *data, size_t len) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i) out += ' ';
        out += digits[(data[i] >> 4) & 0x0F];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

std::vector<DatagramFrame> encode_datagram(const uint8_t *payload, size_t len) {
    std::vector<DatagramFrame> frames;
    if (len <= 7) {
        DatagramFrame frame;
        frame.data[0] = DG_SINGLE;
        if (len) std::copy(payload, payload + len, frame.data.begin() + 1);
        frame.len = static_cast<uint8_t>(len + 1);
        frames.push_back(frame);
        return frames;
    }

    const uint32_t crc = crc32_ieee(payload, len);
    DatagramFrame start;
    start.data[0] = DG_START;
    start.data[1] = static_cast<uint8_t>(crc & 0xFF);
    start.data[2] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    start.data[3] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    start.data[4] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    const size_t first_len = std::min<size_t>(3, len);
    std::copy(payload, payload + first_len, start.data.begin() + 5);
    start.len = static_cast<uint8_t>(first_len + 5);
    frames.push_back(start);

    size_t offset = first_len;
    while (offset < len) {
        const size_t chunk_len = std::min<size_t>(7, len - offset);
        DatagramFrame frame;
        frame.data[0] = (offset + chunk_len >= len) ? DG_END : DG_MIDDLE;
        std::copy(payload + offset, payload + offset + chunk_len,
                  frame.data.begin() + 1);
        frame.len = static_cast<uint8_t>(chunk_len + 1);
        frames.push_back(frame);
        offset += chunk_len;
    }
    return frames;
}

std::vector<DatagramFrame> encode_datagram(const std::string &payload) {
    return encode_datagram(reinterpret_cast<const uint8_t *>(payload.data()),
                           payload.size());
}

DatagramRx::DatagramRx()
    : initial_reserve_(AC_DG_INITIAL_RESERVE_BYTES) {}

DatagramRx::DatagramRx(size_t initial_reserve)
    : initial_reserve_(clamp_initial_reserve(initial_reserve)) {}

DatagramRx::~DatagramRx() {
    free_reassembly_bytes(parts_);
}

bool DatagramRx::reserve_initial() {
    DatagramFeedResult result;
    return reserve_parts(initial_reserve_, result);
}

void DatagramRx::reset() {
    parts_len_ = 0;
    expected_crc_ = 0;
    last_frame_ms_ = 0;
    have_crc_ = false;
}

void DatagramRx::note_frame(uint32_t now_ms) {
    if (now_ms) last_frame_ms_ = now_ms;
}

DatagramFeedResult DatagramRx::poll(uint32_t now_ms) {
    DatagramFeedResult result;
    if (!have_crc_ || !now_ms || !last_frame_ms_) return result;
    if (static_cast<int32_t>(now_ms - last_frame_ms_) <=
        static_cast<int32_t>(AC_DG_IDLE_TIMEOUT_MS)) {
        return result;
    }

    const size_t len = parts_len_;
    reset();
    result.status = DatagramStatus::Error;
    char buf[96];
    snprintf(buf, sizeof(buf), "reassembly timeout len=%u",
             static_cast<unsigned>(len));
    result.error = buf;
    return result;
}

bool DatagramRx::reserve_parts(size_t capacity, DatagramFeedResult &result) {
    if (capacity <= parts_capacity_) return true;
    if (capacity > AC_DG_MAX_PAYLOAD_BYTES) capacity = AC_DG_MAX_PAYLOAD_BYTES;

    uint8_t *next = static_cast<uint8_t *>(alloc_reassembly_bytes(capacity));
    if (!next) {
        reset();
        result.status = DatagramStatus::Error;
        result.error = "datagram allocation failed";
        return false;
    }
    if (parts_ && parts_len_) memcpy(next, parts_, parts_len_);
    free_reassembly_bytes(parts_);
    parts_ = next;
    parts_capacity_ = capacity;
    return true;
}

bool DatagramRx::append_bytes(const uint8_t *data,
                              size_t len,
                              DatagramFeedResult &result) {
    if (!len) return true;
    if (len > AC_DG_MAX_PAYLOAD_BYTES ||
        parts_len_ > AC_DG_MAX_PAYLOAD_BYTES - len) {
        reset();
        result.status = DatagramStatus::Error;
        result.error = "datagram exceeds max payload";
        return false;
    }

    const size_t needed = parts_len_ + len;
    if (needed > parts_capacity_) {
        size_t target = parts_capacity_;
        if (target < initial_reserve_) {
            target = initial_reserve_;
        }
        while (target < needed && target < AC_DG_MAX_PAYLOAD_BYTES) {
            target *= 2;
        }
        if (target > AC_DG_MAX_PAYLOAD_BYTES) {
            target = AC_DG_MAX_PAYLOAD_BYTES;
        }
        if (!reserve_parts(target, result)) return false;
    }

    memcpy(parts_ + parts_len_, data, len);
    parts_len_ += len;
    return true;
}

void DatagramRx::set_payload_view(DatagramFeedResult &result) const {
    result.payload_data = reinterpret_cast<const char *>(parts_);
    result.payload_len = parts_len_;
}

DatagramFeedResult DatagramRx::feed(const uint8_t *data,
                                    size_t len,
                                    uint32_t now_ms) {
    DatagramFeedResult result;
    if (!data || len == 0) return result;

    const uint8_t flag = data[0] & DG_FLAG_MASK;
    DatagramFeedResult timeout = poll(now_ms);
    if (timeout.status == DatagramStatus::Error &&
        flag != DG_START &&
        flag != DG_SINGLE) {
        return timeout;
    }

    switch (flag) {
        case DG_SINGLE:
            reset();
            result.status = DatagramStatus::Complete;
            result.payload_data = reinterpret_cast<const char *>(data + 1);
            result.payload_len = len - 1;
            return result;

        case DG_START:
            if (len < 5) {
                reset();
                result.status = DatagramStatus::Error;
                result.error = "start frame shorter than CRC field";
                return result;
            }
            parts_len_ = 0;
            expected_crc_ = static_cast<uint32_t>(data[1]) |
                            (static_cast<uint32_t>(data[2]) << 8) |
                            (static_cast<uint32_t>(data[3]) << 16) |
                            (static_cast<uint32_t>(data[4]) << 24);
            have_crc_ = true;
            note_frame(now_ms);
            append_bytes(data + 5, len - 5, result);
            return result;

        case DG_MIDDLE:
            if (!have_crc_) return result;
            note_frame(now_ms);
            append_bytes(data + 1, len - 1, result);
            return result;

        case DG_END: {
            if (!have_crc_) return result;
            note_frame(now_ms);
            if (!append_bytes(data + 1, len - 1, result)) return result;
            const uint32_t actual = crc32_ieee(parts_, parts_len_);
            if (actual != expected_crc_) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "CRC mismatch expected=0x%08lX actual=0x%08lX len=%u",
                         static_cast<unsigned long>(expected_crc_),
                         static_cast<unsigned long>(actual),
                         static_cast<unsigned>(parts_len_));
                reset();
                result.status = DatagramStatus::Error;
                result.error = buf;
                return result;
            }
            result.status = DatagramStatus::Complete;
            set_payload_view(result);
            have_crc_ = false;
            expected_crc_ = 0;
            last_frame_ms_ = 0;
            return result;
        }

        default:
            reset();
            result.status = DatagramStatus::Error;
            result.error = "unknown datagram flag";
            return result;
    }
}

}  // namespace aircannect
