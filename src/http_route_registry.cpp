#include "http_route_registry.h"

#include <utility>

#include "memory_manager.h"

namespace aircannect {
namespace {

class PsramWebHandler final : public AsyncCallbackWebHandler {
public:
    static void *operator new(size_t size) noexcept {
        return Memory::alloc_large(size, false);
    }

    // AsyncWebServer deletes through AsyncWebHandler's virtual destructor.
    static void operator delete(void *memory) noexcept {
        Memory::free(memory);
    }
};

}  // namespace

void HttpRouteRegistry::on(AsyncURIMatcher uri,
                           WebRequestMethodComposite method,
                           ArRequestHandlerFunction request,
                           ArUploadHandlerFunction upload,
                           ArBodyHandlerFunction body) {
    if (!ready_) return;

    auto *handler = new PsramWebHandler();
    if (!handler) {
        ready_ = false;
        return;
    }

    handler->setUri(std::move(uri));
    handler->setMethod(std::move(method));
    handler->onRequest(std::move(request));
    handler->onUpload(std::move(upload));
    handler->onBody(std::move(body));
    server_.addHandler(handler);
}

}  // namespace aircannect
