#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

#include "background_worker.h"
#include "board.h"

namespace aircannect {

enum class StorageDiagnosticState : uint8_t {
    Idle,
    Queued,
    Writing,
    Complete,
    Failed,
};

struct StorageDiagnosticStatus {
    bool available = false;
    StorageDiagnosticState state = StorageDiagnosticState::Idle;
    size_t bytes = 0;
    char path[AC_STORAGE_WRITE_PATH_MAX] = {};
    char error[64] = {};
};

class StorageDiagnosticJob : public BackgroundJob {
public:
    void begin();

    bool request_append(const char *path, const uint8_t *data, size_t len);
    StorageDiagnosticStatus status() const;

    const char *name() const override { return "storage_test"; }
    JobStep step() override;

private:
    static constexpr size_t PAYLOAD_CAPACITY = 512;

    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;

    mutable StaticSemaphore_t lock_storage_ = {};
    mutable SemaphoreHandle_t lock_ = nullptr;

    uint8_t *payload_ = nullptr;
    size_t payload_len_ = 0;
    StorageDiagnosticStatus status_ = {};
};

const char *storage_diagnostic_state_name(StorageDiagnosticState state);

}  // namespace aircannect
