#include "http_response_utils.h"

#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <new>
#include <string.h>

#include "large_byte_buffer.h"
#include "large_text_buffer.h"

namespace aircannect {

bool http_prepare_json_response(AsyncWebServerRequest *request,
                                const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response) {
    (void)request;
    return http_prepare_json_response(json, response);
}

bool http_prepare_json_response(const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response) {
    response = nullptr;

    try {
        if (json.length() == 0) {
            response = new (std::nothrow) AsyncBasicResponse(200, "application/json", "");
            return response != nullptr;
        }

        // AsyncResponseStream copies the entire payload with interrupts masked.
        // Keep an independent snapshot while later callbacks send it in pieces.
        const auto payload = LargeByteBuffer::copy_and_freeze(
            json.c_str(), json.length());
        if (!payload) return false;

        response = new (std::nothrow) AsyncCallbackResponse(
            "application/json", json.length(),
            [payload](uint8_t *buffer, size_t capacity, size_t offset) -> size_t {
                if (offset >= payload->size()) return 0;

                const size_t bytes = std::min(capacity, payload->size() - offset);
                memcpy(buffer, payload->data() + offset, bytes);
                return bytes;
            });
        return response != nullptr;
    } catch (const std::bad_alloc &) {
        return false;
    }
}

}  // namespace aircannect
