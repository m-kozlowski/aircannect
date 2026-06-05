#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

namespace aircannect {

enum class StorageType : uint8_t {
    None,
    SdMmc,
    SpiSd,
};

enum class StorageState : uint8_t {
    Disabled,
    NotPresent,
    Mounted,
    Error,
};

struct StorageStatus {
    bool configured = false;
    bool mounted = false;
    StorageType type = StorageType::None;
    StorageState state = StorageState::Disabled;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t card_size_bytes = 0;
    uint32_t last_checked_ms = 0;
    uint8_t width = 0;
    char mount_point[16] = {};
    char card_type[16] = {};
    char last_error[96] = {};
};

namespace Storage {

void begin();
void poll();
bool remount();

StorageStatus status();
bool mounted();
bool ensure_dir(const char *path);
bool exists(const char *path);
bool remove(const char *path);
bool rmdir(const char *path);
bool rename(const char *from, const char *to);
File open(const char *path, const char *mode);

void lock();
void unlock();

class Guard {
public:
    Guard() { lock(); }
    ~Guard() { unlock(); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
};

const char *type_name(StorageType type);
const char *state_name(StorageState state);

}  // namespace Storage
}  // namespace aircannect
