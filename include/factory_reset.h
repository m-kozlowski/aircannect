#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class FirmwareInstaller;
class CrashDiagnostics;

namespace FactoryReset {

enum class Scope : uint8_t { Storage = 1, All = 2 };
enum class State : uint8_t { Idle, Queued, Succeeded, Failed };

struct Status {
    Scope scope = Scope::Storage;
    State state = State::Idle;
    bool sd_formatted = false;
    bool nvs_erased = false;
    bool crash_cleared = false;
    char error[96] = {};
};

using BlockedCallback = const char *(*)();

// Main-loop owner; the intent is saved only after reboot quiescence.
void begin(FirmwareInstaller &installer, BlockedCallback blocked);
bool request(Scope scope, char *error, size_t error_size);
Status status();
const char *state_name(State state);

// Before config, radios, and storage start. False means boot must stop.
bool run_pending_on_boot(CrashDiagnostics &crash);

}  // namespace FactoryReset
}  // namespace aircannect
