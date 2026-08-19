#include "http_response_utils.h"

#include <ESPAsyncWebServer.h>

#include "large_text_buffer.h"

namespace aircannect {

bool http_prepare_json_response(AsyncWebServerRequest *request,
                                const LargeTextBuffer &json,
                                AsyncResponseStream *&response) {
    response = request->beginResponseStream("application/json");
    if (!response) return false;

    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    return true;
}

}  // namespace aircannect
