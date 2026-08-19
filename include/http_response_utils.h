#pragma once

class AsyncResponseStream;
class AsyncWebServerRequest;

namespace aircannect {

class LargeTextBuffer;

bool http_prepare_json_response(AsyncWebServerRequest *request,
                                const LargeTextBuffer &json,
                                AsyncResponseStream *&response);

}  // namespace aircannect
