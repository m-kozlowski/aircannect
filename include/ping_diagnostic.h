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
    struct Reply {
        uint16_t sequence = 0;
        bool received = false;
        uint32_t elapsed_ms = 0;
    };

    bool start(const char *host, uint16_t count);
    void poll();
    bool take_reply(Reply &reply);
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
    void record_reply(bool received, uint32_t elapsed_ms);

    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<bool> cancelled_{false};
    Result result_;
    uint16_t count_ = DefaultCount;
    Reply replies_[MaxCount] = {}; // ping replies buffered here
    std::atomic<uint16_t> reply_count_{0};
    uint16_t reply_index_ = 0;
};

}  // namespace aircannect
