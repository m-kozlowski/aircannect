#include <algorithm>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" BaseType_t __real_xTaskCreatePinnedToCore(
    TaskFunction_t code, const char *name, uint32_t stack, void *arg,
    UBaseType_t priority, TaskHandle_t *handle, BaseType_t core);

extern "C" BaseType_t __wrap_xTaskCreatePinnedToCore(
    TaskFunction_t code, const char *name, uint32_t stack, void *arg,
    UBaseType_t priority, TaskHandle_t *handle, BaseType_t core) {
    // Runs before setup(): no logging, allocation, or application state here.
    // Stock 1024-byte IPC stacks can overflow during BT interrupt allocation.
    if (name && (strcmp(name, "ipc0") == 0 || strcmp(name, "ipc1") == 0)) {
        stack = std::max<uint32_t>(stack, 2048);
    }

    return __real_xTaskCreatePinnedToCore(
        code, name, stack, arg, priority, handle, core);
}
