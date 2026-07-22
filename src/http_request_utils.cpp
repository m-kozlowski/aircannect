#include "http_request_utils.h"

#include <ESPAsyncWebServer.h>

#include <stdlib.h>
#include <string.h>

#include "board_net.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

void discard_request_body(AsyncWebServerRequest *request) {
    if (!request || !request->_tempObject) return;

    Memory::free(request->_tempObject);
    request->_tempObject = nullptr;
}

std::string take_request_body(AsyncWebServerRequest *request) {
    std::string body;
    if (request && request->_tempObject) {
        body = static_cast<const char *>(request->_tempObject);
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
    if (index == 0) {
        discard_request_body(request);
        if (total > AC_WEB_MAX_POST_BODY) return;

        request->_tempObject = Memory::calloc_large(total + 1, sizeof(char));
    }

    char *body = static_cast<char *>(request->_tempObject);
    if (!body || index + length > AC_WEB_MAX_POST_BODY) return;

    memcpy(body + index, data, length);
    body[index + length] = 0;
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
