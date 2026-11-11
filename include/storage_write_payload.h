#pragma once

#include "checked_size.h"
#include "large_byte_buffer.h"
#include "large_scratch_array.h"

namespace aircannect {

using StorageWriteBuffers =
    LargeScratchArray<std::shared_ptr<const LargeByteBuffer>>;

struct StorageWritePayload {
    struct Cursor {
        // bytes is buffer 0; buffers start at 1.
        size_t buffer = 0;
        size_t offset = 0;
    };

    std::shared_ptr<const LargeByteBuffer> bytes;
    // Optional buffers following bytes, in order. No copy into a flat payload.
    std::shared_ptr<const StorageWriteBuffers> buffers;

    size_t size() const {
        size_t total = bytes ? bytes->size() : 0;
        if (buffers) {
            for (size_t i = 0; i < buffers->size(); ++i) {
                const auto &part = buffers->data()[i];
                if (!part || !CheckedSize::add_to(total, part->size())) {
                    return 0;
                }
            }
        }

        return total;
    }

    const std::shared_ptr<const LargeByteBuffer> &span(Cursor &cursor) const {
        static const std::shared_ptr<const LargeByteBuffer> empty;

        while (cursor.buffer == 0 ||
               (buffers && cursor.buffer - 1 < buffers->size())) {
            const auto &part = cursor.buffer == 0
                ? bytes : buffers->data()[cursor.buffer - 1];

            if (part) {
                if (cursor.offset < part->size()) return part;

                cursor.offset -= part->size();
            }
            ++cursor.buffer;
        }

        return empty;
    }

    const uint8_t *span(size_t position, size_t &length) const {
        Cursor cursor{0, position};
        const auto &part = span(cursor);
        length = part ? part->size() - cursor.offset : 0;
        return part ? part->data() + cursor.offset : nullptr;
    }

    explicit operator bool() const { return bytes || buffers; }

    void reset() {
        bytes.reset();
        buffers.reset();
    }
};

}  // namespace aircannect
