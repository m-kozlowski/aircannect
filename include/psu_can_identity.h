#pragma once

namespace aircannect {

class CanRpcLink;

class PsuCanIdentity {
public:
    explicit PsuCanIdentity(CanRpcLink &can) : can_(can) {}

    void set_enabled(bool enabled);
    void note_device_boot();
    void poll(bool suspended);

private:
    CanRpcLink &can_;
    bool enabled_ = false;
    bool pending_ = false;
    bool failure_logged_ = false;
};

}  // namespace aircannect
