#pragma once

#include <ESPAsyncWebServer.h>

#include <atomic>
#include <memory>

namespace aircannect {

// The producer publishes a response, never a request pointer. Only AsyncTCP
// starts and advances that response, including when preparation ends late.
class AsyncDeferredResponse final : public AsyncWebServerResponse {
public:
    class State {
    public:
        ~State();

        bool publish(std::unique_ptr<AsyncWebServerResponse> response);
        bool cancelled() const { return cancelled_.load(); }

    private:
        friend class AsyncDeferredResponse;
        void cancel();
        std::unique_ptr<AsyncWebServerResponse> take();

        std::atomic<bool> cancelled_{false};
        std::atomic<AsyncWebServerResponse *> ready_{nullptr};
    };

    explicit AsyncDeferredResponse(std::shared_ptr<State> state);
    ~AsyncDeferredResponse() override;

    bool _sourceValid() const override;
    bool _started() const override;
    bool _finished() const override;
    bool _failed() const override;
    void _respond(AsyncWebServerRequest *request) override;
    size_t _ack(AsyncWebServerRequest *request,
                size_t len,
                uint32_t time) override;

private:
    void start_ready(AsyncWebServerRequest *request);

    std::shared_ptr<State> state_;
    std::unique_ptr<AsyncWebServerResponse> response_;
    bool failed_ = false;
};

}  // namespace aircannect
