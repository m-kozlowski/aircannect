#pragma once

#include <ESPAsyncWebServer.h>

namespace aircannect {

// Registers PSRAM-backed handlers owned and destroyed by AsyncWebServer.
class HttpRouteRegistry {
public:
    explicit HttpRouteRegistry(AsyncWebServer &server) : server_(server) {}

    void on(AsyncURIMatcher uri,
            WebRequestMethodComposite method,
            ArRequestHandlerFunction request,
            ArUploadHandlerFunction upload = nullptr,
            ArBodyHandlerFunction body = nullptr);
    bool ready() const { return ready_; }

private:
    AsyncWebServer &server_;
    bool ready_ = true;
};

}  // namespace aircannect
