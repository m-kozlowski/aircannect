#include "http_request_utils.h"

#include <ESPAsyncWebServer.h>

#include <stdlib.h>
#include <string.h>

#include "board_net.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

struct RequestBodyState {
    char bytes[AC_WEB_MAX_POST_BODY + 1] = {};
    size_t length = 0;
    size_t total = 0;
    bool complete = false;
};

void discard_request_body(AsyncWebServerRequest *request) {
    if (!request || !request->_tempObject) return;

    Memory::free(request->_tempObject);
    request->_tempObject = nullptr;
}

std::string take_request_body(AsyncWebServerRequest *request) {
    std::string body;
    if (request && request->_tempObject) {
        const RequestBodyState *state =
            static_cast<const RequestBodyState *>(request->_tempObject);
        if (!state->complete || state->length > AC_WEB_MAX_POST_BODY) {
            discard_request_body(request);
            return body;
        }

        body.assign(state->bytes, state->length);
    }

    discard_request_body(request);
    return body;
}

}  // namespace

void http_request_body_handler(AsyncWebServerRequest *request,
                               uint8_t *data,
                               size_t length,
                               size_t index,
                               size_t total) {
    if (!request) return;

    if (index == 0) {
        discard_request_body(request);
        if (total > AC_WEB_MAX_POST_BODY) return;

        request->_tempObject = Memory::calloc_large(
            1, sizeof(RequestBodyState));
        if (request->_tempObject) {
            static_cast<RequestBodyState *>(request->_tempObject)->total =
                total;
        }
    }

    RequestBodyState *state =
        static_cast<RequestBodyState *>(request->_tempObject);
    if (!state) return;

    const bool declared_length_fits =
        total == 0 || (index <= total && length <= total - index);
    if ((state->complete && length > 0) || state->total != total ||
        index != state->length || index > AC_WEB_MAX_POST_BODY ||
        length > AC_WEB_MAX_POST_BODY - index || !declared_length_fits ||
        (length > 0 && (!data || memchr(data, 0, length))) ||
        (length == 0 && total > 0 && index < total)) {
        discard_request_body(request);
        return;
    }

    if (length > 0) memcpy(state->bytes + index, data, length);
    state->length = index + length;
    state->bytes[state->length] = 0;
    state->complete = total > 0 ? state->length == total : length == 0;
}

void http_discard_request_body(AsyncWebServerRequest *request) {
    discard_request_body(request);
}

bool http_parse_json_body(AsyncWebServerRequest *request,
                          JsonDocument &document,
                          std::string &body) {
    body = take_request_body(request);
    if (body.empty()) return false;

    return !deserializeJson(document, body.c_str());
}

bool http_size_arg(AsyncWebServerRequest *request,
                   const char *name,
                   size_t default_value,
                   size_t max_value,
                   size_t &out) {
    out = default_value;
    if (!request || !name || !request->hasArg(name)) return true;

    const String value = request->arg(name);
    char *end = nullptr;
    const unsigned long long parsed = strtoull(value.c_str(), &end, 10);
    if (!end || *end != 0 || parsed > max_value) return false;

    out = static_cast<size_t>(parsed);
    return true;
}

bool http_bool_arg(AsyncWebServerRequest *request,
                   const char *name,
                   bool default_value) {
    if (!request || !name || !request->hasArg(name)) return default_value;

    String value = request->arg(name);
    value.trim();
    value.toLowerCase();
    return value != "0" && value != "false" && value != "off" &&
           value != "no";
}

}  // namespace aircannect
