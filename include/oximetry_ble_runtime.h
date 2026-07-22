#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace aircannect {

class OximetryBleRuntime {
public:
    bool begin();
    bool ensure_started(const char *name);

private:
    SemaphoreHandle_t mutex_ = nullptr;
    StaticSemaphore_t mutex_storage_ = {};
};

}  // namespace aircannect
