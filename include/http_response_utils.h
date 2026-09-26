#pragma once

class AsyncWebServerResponse;

namespace aircannect {

class LargeTextBuffer;

bool http_prepare_json_response(const LargeTextBuffer &json,
                                AsyncWebServerResponse *&response);

}  // namespace aircannect
