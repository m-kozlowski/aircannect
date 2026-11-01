#include "http_response_utils.h"

#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <memory>
#include <new>
#include <string.h>

#include "large_allocator.h"
#include "large_text_buffer.h"

namespace aircannect {

bool http_prepare_json_response(const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response) {
    response = nullptr;
    LargeTextBuffer snapshot;
    if (json.overflowed() || !snapshot.reserve_exact(json.length()) ||
        !snapshot.append(json.c_str(), json.length())) return false;

    return http_prepare_json_response(std::move(snapshot), response);
}

bool http_prepare_json_response(LargeTextBuffer &&json,
                                AsyncWebServerResponse *&response) {
    response = nullptr;
    if (json.overflowed()) return false;

    try {
        if (json.length() == 0) {
            response = new (std::nothrow) AsyncBasicResponse(200, "application/json", "");
            return response != nullptr;
        }

        // AsyncResponseStream copies the entire payload with interrupts masked.
        // Transfer the snapshot while later callbacks send it in pieces.
        const auto payload = std::allocate_shared<LargeTextBuffer>(
            LargeAllocator<LargeTextBuffer>());
        payload->swap(json);

        response = new (std::nothrow) AsyncCallbackResponse(
            "application/json", payload->length(),
            [payload](uint8_t *buffer, size_t capacity, size_t offset) -> size_t {
                if (offset >= payload->length()) return 0;

                const size_t bytes = std::min(capacity, payload->length() - offset);
                memcpy(buffer, payload->c_str() + offset, bytes);
                return bytes;
            });
        return response != nullptr;
    } catch (const std::bad_alloc &) {
        return false;
    }
}

}  // namespace aircannect
