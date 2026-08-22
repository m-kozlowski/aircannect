#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace aircannect {

class BleRuntime {
public:
    class ScanLease {
    public:
        ScanLease() = default;
        ScanLease(const ScanLease &) = delete;
        ScanLease &operator=(const ScanLease &) = delete;
        ScanLease(ScanLease &&other) noexcept;
        ScanLease &operator=(ScanLease &&other) noexcept;
        ~ScanLease();

        explicit operator bool() const { return runtime_ != nullptr; }
        void release();

    private:
        friend class BleRuntime;
        explicit ScanLease(BleRuntime *runtime) : runtime_(runtime) {}

        BleRuntime *runtime_ = nullptr;
    };

    bool begin();
    bool ensure_started(const char *name);
    ScanLease acquire_scan(TickType_t timeout_ticks);
    bool scan_in_progress() const;

private:
    void release_scan();

    SemaphoreHandle_t init_mutex_ = nullptr;
    StaticSemaphore_t init_mutex_storage_ = {};
    SemaphoreHandle_t scan_mutex_ = nullptr;
    StaticSemaphore_t scan_mutex_storage_ = {};
};

}  // namespace aircannect
