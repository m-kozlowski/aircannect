#pragma once

#include <atomic>
#include <stdint.h>

class AsyncWebServerRequest;

namespace aircannect {

class ReportTask;
class StorageStreamPort;

// Presents immutable report snapshots and artifacts over HTTP. Report policy
// stays in ReportTask; this class only validates requests and streams bytes.
class ReportHttpController {
public:
    void begin(ReportTask &report_task, StorageStreamPort &stream_port);

    void send_summary(AsyncWebServerRequest *request) const;
    void send_result(AsyncWebServerRequest *request) const;
    void send_plot(AsyncWebServerRequest *request) const;

private:
    uint32_t next_generation() const;

    ReportTask *report_task_ = nullptr;
    StorageStreamPort *stream_port_ = nullptr;
    mutable std::atomic<uint32_t> next_generation_{1};
};

}  // namespace aircannect
