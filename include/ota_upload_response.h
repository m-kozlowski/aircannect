#pragma once

#include <ESPAsyncWebServer.h>

namespace aircannect {

class OtaUploadResponse final : public AsyncBasicResponse {
public:
    explicit OtaUploadResponse(const String &json)
        : AsyncBasicResponse(200, "application/json", json) {}

    bool _finished() const override {
        // The base response finishes when bytes are queued, before TCP ACKs.
        return _failed() ||
            (AsyncBasicResponse::_finished() && _ackedLength >= _writtenLength);
    }
};

}  // namespace aircannect
