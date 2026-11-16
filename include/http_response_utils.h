#pragma once

#include <memory>

class AsyncWebServerResponse;

namespace aircannect {

class LargeTextBuffer;

bool http_prepare_json_response(const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response);
bool http_prepare_json_response(LargeTextBuffer &&json,
                                AsyncWebServerResponse *&response);

std::unique_ptr<AsyncWebServerResponse> http_json_error_response(
    int status, const char *error);

}  // namespace aircannect
