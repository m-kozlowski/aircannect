#pragma once

class AsyncWebServerResponse;
class AsyncWebServerRequest;

namespace aircannect {

class LargeTextBuffer;

bool http_prepare_json_response(AsyncWebServerRequest *request,
                                const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response);
bool http_prepare_json_response(const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response);

}  // namespace aircannect
