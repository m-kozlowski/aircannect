#pragma once

class AsyncWebServer;

namespace aircannect {

class HttpRouteModule {
public:
    virtual ~HttpRouteModule() = default;

    virtual void register_routes(AsyncWebServer &server) = 0;
};

}  // namespace aircannect
