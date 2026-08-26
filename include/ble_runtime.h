#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "board.h"

namespace aircannect {

struct BleAdvertisement {
    char address[18] = {};
    uint8_t address_type = 0;
    int rssi = 0;
};

using BleAdvertisementHandler = void (*)(
    void *context, const BleAdvertisement &advertisement);

class BleRuntimeScanCallbacks;

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

    void set_passive_observer(BleAdvertisementHandler handler,
                              void *context);
    void request_passive_observation(bool enabled);
    bool passive_observation_active() const;

private:
    friend class BleRuntimeScanCallbacks;

    void release_scan();
    bool start_passive_observer_locked();
    bool stop_passive_observer_locked();
    void note_advertisement(const void *device);
    void note_observer_stopped();

    SemaphoreHandle_t init_mutex_ = nullptr;
    StaticSemaphore_t init_mutex_storage_ = {};
    SemaphoreHandle_t scan_mutex_ = nullptr;
    StaticSemaphore_t scan_mutex_storage_ = {};

    mutable portMUX_TYPE observer_mux_ = portMUX_INITIALIZER_UNLOCKED;
    BleAdvertisementHandler observer_handler_ = nullptr;
    void *observer_context_ = nullptr;
    bool observer_requested_ = false;
    bool observer_running_ = false;
    uint32_t observer_retry_ms_ = 0;
#if AC_BLE_ENABLED
    BleRuntimeScanCallbacks *observer_callbacks_ = nullptr;
#endif
};

}  // namespace aircannect
