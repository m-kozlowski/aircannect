#pragma once

namespace aircannect {

class HttpRouteRegistry;

class HttpRouteModule {
public:
    virtual ~HttpRouteModule() = default;

    virtual void register_routes(HttpRouteRegistry &server) = 0;
};

}  // namespace aircannect
