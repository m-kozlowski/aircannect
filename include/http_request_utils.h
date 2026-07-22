#pragma once

#include <ArduinoJson.h>

#include <stddef.h>
#include <stdint.h>
#include <string>

class AsyncWebServerRequest;

namespace aircannect {

void http_request_body_handler(AsyncWebServerRequest *request,
                               uint8_t *data,
                               size_t length,
                               size_t index,
                               size_t total);
void http_discard_request_body(AsyncWebServerRequest *request);

bool http_parse_json_body(AsyncWebServerRequest *request,
                          JsonDocument &document,
                          std::string &body);

bool http_size_arg(AsyncWebServerRequest *request,
                   const char *name,
                   size_t default_value,
                   size_t max_value,
                   size_t &out);

bool http_bool_arg(AsyncWebServerRequest *request,
                   const char *name,
                   bool default_value);

}  // namespace aircannect
