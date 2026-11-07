#pragma once

#include <atomic>
#include <ping/ping_sock.h>

namespace aircannect {

class PingDiagnostic {
public:
    static constexpr uint16_t DefaultCount = 4, MaxCount = 10;
    static constexpr uint32_t ResolveTimeoutMs = 10000;
    struct Result {
        ip_addr_t address = {};
        const char *error = nullptr;
        uint32_t sent = 0, received = 0;
        uint32_t min_ms = UINT32_MAX, max_ms = 0, sum_ms = 0;
    };

    bool start(const char *host, uint16_t count);
    void poll();
    void cancel() { cancelled_.store(true); }
    bool resolving() const { return phase_.load() == Phase::Resolving; }
    bool finished() const { return phase_.load() == Phase::Done; }
    // read only after finished(): callbacks publish the completed result.
    const Result &result() const { return result_; }

private:
    enum class Phase { Idle, Resolving, Ready, Running, Done };
    static void dns_done(const char *, const ip_addr_t *address, void *arg);
    static void reply(esp_ping_handle_t handle, void *arg);
    static void timeout(esp_ping_handle_t handle, void *arg);
    static void done(esp_ping_handle_t handle, void *arg);
    void fail(const char *error) { result_.error = error; phase_.store(Phase::Done); }

    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<bool> cancelled_{false};
    Result result_;
    uint16_t count_ = DefaultCount;
};

}  // namespace aircannect
