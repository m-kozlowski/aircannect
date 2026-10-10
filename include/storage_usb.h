#pragma once

#include <stdint.h>

namespace aircannect {

enum class StorageUsbState : uint8_t {
    Local, Preparing, Shared, Returning, Restoring, Error
};

struct StorageUsbStatus {
    StorageUsbState state = StorageUsbState::Local;
    bool supported = false;
    uint32_t revision = 0;
    char error[64] = {};
};

namespace StorageUsb {

// Main stops producers; the storage task drains and closes accepted work.
bool request(bool enabled);
void reject_entry(const char *error);
StorageUsbStatus status();
bool suspended();
void set_entry_allowed(bool allowed);
void set_producers_stopped(bool stopped);
bool producers_stopped();

// Storage-task lifecycle and raw-sector service.
void begin(void (*wake_storage)());
bool share();
bool poll();
void restored(bool success);
void fail(const char *error);
void resume_local();
void seal_local_requests();
const char *state_name(StorageUsbState state);

}  // namespace StorageUsb
}  // namespace aircannect
