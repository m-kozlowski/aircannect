#include "async_deferred_response.h"

#include <utility>

namespace aircannect {

AsyncDeferredResponse::State::~State() { cancel(); }

bool AsyncDeferredResponse::State::publish(
    std::unique_ptr<AsyncWebServerResponse> response) {
    if (!response || cancelled()) return false;

    AsyncWebServerResponse *expected = nullptr;
    AsyncWebServerResponse *raw = response.release();
    if (!ready_.compare_exchange_strong(expected, raw)) {
        delete raw;
        return false;
    }

    // Disconnect may have won immediately before publication.
    if (cancelled()) {
        delete ready_.exchange(nullptr);
        return false;
    }
    return true;
}

void AsyncDeferredResponse::State::cancel() {
    cancelled_.store(true);
    delete ready_.exchange(nullptr);
}

std::unique_ptr<AsyncWebServerResponse>
AsyncDeferredResponse::State::take() {
    return std::unique_ptr<AsyncWebServerResponse>(ready_.exchange(nullptr));
}

AsyncDeferredResponse::AsyncDeferredResponse(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

AsyncDeferredResponse::~AsyncDeferredResponse() {
    if (state_) state_->cancel();
}

bool AsyncDeferredResponse::_sourceValid() const { return state_ != nullptr; }

bool AsyncDeferredResponse::_started() const {
    return response_ && response_->_started();
}

bool AsyncDeferredResponse::_finished() const {
    return failed_ || (response_ && response_->_finished());
}

bool AsyncDeferredResponse::_failed() const {
    return failed_ || (response_ && response_->_failed());
}

void AsyncDeferredResponse::start_ready(AsyncWebServerRequest *request) {
    if (response_ || !state_ || failed_) return;

    response_ = state_->take();
    if (!response_) return;

    if (!response_->_sourceValid()) {
        failed_ = true;
        if (request && request->client()) request->client()->abort();
        return;
    }

    _code = response_->code();
    response_->_respond(request);
}

void AsyncDeferredResponse::_respond(AsyncWebServerRequest *request) {
    start_ready(request);
}

size_t AsyncDeferredResponse::_ack(AsyncWebServerRequest *request,
                                   size_t len,
                                   uint32_t time) {
    if (!response_) {
        start_ready(request);
        return 0;
    }

    return failed_ ? 0 : response_->_ack(request, len, time);
}

}  // namespace aircannect
