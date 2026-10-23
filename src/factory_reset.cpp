#include "factory_reset.h"

#include <stdio.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "board.h"
#include "crash_diagnostics.h"
#include "debug_log.h"
#include "firmware_installer.h"
#include "storage_internal.h"
#include "storage_usb.h"
#include "string_util.h"

namespace aircannect::FactoryReset {
namespace {

constexpr const char *Namespace = "factory_reset";
constexpr const char *PendingKey = "pending";
FirmwareInstaller *installer = nullptr;
BlockedCallback runtime_blocked = nullptr;
Status snapshot;

void failed(const char *step, const char *error) {
    snapshot.state = State::Failed;
    if (!snapshot.error[0]) {
        snprintf(snapshot.error, sizeof(snapshot.error), "%s: %s", step, error);
    }
    Log::logf(CAT_GENERAL, LOG_ERROR, "[RESET] %s failed error=%s\n", step, error);
}

bool prepare_reboot(void *) {
    const char *blocked = StorageUsb::suspended() ? "usb_storage_active" :
        runtime_blocked ? runtime_blocked() : "not_initialized";
    if (blocked) {
        failed("prepare", blocked);
        return false;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(Namespace, NVS_READWRITE, &handle);
    esp_err_t cleanup = ESP_OK;
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, PendingKey, static_cast<uint8_t>(snapshot.scope));
        if (result == ESP_OK) result = nvs_commit(handle);
        if (result != ESP_OK) {
            // A failed commit does not guarantee the set was rolled back.
            cleanup = nvs_erase_key(handle, PendingKey);
            if (cleanup == ESP_OK) cleanup = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if (result == ESP_OK) return true;

    failed("save intent", esp_err_to_name(result));
    if (cleanup != ESP_OK && cleanup != ESP_ERR_NVS_NOT_FOUND) {
        // The confirmed intent may still be armed. Resolve it at boot before
        // allowing normal work, rather than leaving a surprise for a later boot.
        failed("disarm intent; restarting", esp_err_to_name(cleanup));
        return true;
    }
    return false;
}

}  // namespace

void begin(FirmwareInstaller &owner, BlockedCallback blocked) {
    installer = &owner;
    runtime_blocked = blocked;
}

bool request(Scope scope, char *error, size_t error_size) {
    const char *blocked = !installer || !runtime_blocked ? "not_initialized" :
        StorageUsb::suspended() ? "usb_storage_active" : runtime_blocked();
#if !AC_STORAGE_SDMMC_ENABLED && !AC_STORAGE_SPI_SD_ENABLED
    if (!blocked && scope == Scope::Storage) blocked = "storage_unsupported";
#endif
    if (!blocked && !installer->request_prepared_reboot(prepare_reboot, nullptr)) {
        blocked = "reboot_busy";
    }
    copy_cstr(error, error_size, blocked);
    if (blocked) return false;

    snapshot = {};
    snapshot.scope = scope;
    snapshot.state = State::Queued;
    return true;
}

Status status() { return snapshot; }

const char *state_name(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::Queued: return "queued";
        case State::Succeeded: return "succeeded";
        case State::Failed: return "failed";
    }
    return "unknown";
}

bool run_pending_on_boot(CrashDiagnostics &crash) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(Namespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) {
        failed("read intent", esp_err_to_name(result));
        return false;
    }

    uint8_t value = 0;
    result = nvs_get_u8(handle, PendingKey, &value);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) {
        failed("read intent", esp_err_to_name(result));
        return false;
    }

    // Consume before destructive work: failures require explicit confirmation
    // again, never an automatic format of a replacement card on the next boot.
    result = nvs_open(Namespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_erase_key(handle, PendingKey);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        failed("consume intent", esp_err_to_name(result));
        return false;
    }
    if (value != static_cast<uint8_t>(Scope::Storage) &&
        value != static_cast<uint8_t>(Scope::All)) {
        failed("intent", "unknown scope; no data erased");
        return true;
    }

    snapshot = {};
    snapshot.scope = static_cast<Scope>(value);
    char error[96] = {};
    snapshot.sd_formatted = Storage::format_card_on_boot(error, sizeof(error));
    if (!snapshot.sd_formatted) failed("SD format", error);

    if (snapshot.scope == Scope::All) {
        error[0] = 0;
        CrashDiagnosticsSnapshot dump;
        snapshot.crash_cleared = crash.copy_snapshot(dump) &&
            ((dump.dump_state == CrashDumpState::Unsupported &&
              !dump.rtc_panic_available) ||
             crash.clear(error, sizeof(error)));

        if (!snapshot.crash_cleared) failed("crash clear", error[0] ? error : "unavailable");

        result = nvs_flash_erase();
        snapshot.nvs_erased = result == ESP_OK;
        if (!snapshot.nvs_erased) failed("NVS erase", esp_err_to_name(result));

        // Erase deinitializes NVS. No config or BLE owner exists yet.
        result = nvs_flash_init();
        if (result != ESP_OK) {
            failed("NVS init", esp_err_to_name(result));
            return false;
        }
    }

    if (snapshot.state != State::Failed) snapshot.state = State::Succeeded;

    const bool all = snapshot.scope == Scope::All;
    Log::logf(CAT_GENERAL, snapshot.state == State::Succeeded ? LOG_INFO : LOG_ERROR,
              "[RESET] %s result=%s sd=%s nvs=%s crash=%s\n",
              all ? "factory reset" : "storage format",
              state_name(snapshot.state), snapshot.sd_formatted ? "cleared" : "failed",
              !all ? "preserved" : snapshot.nvs_erased ? "cleared" : "failed",
              !all ? "preserved" : snapshot.crash_cleared ? "cleared" : "failed");

    return true;
}

}  // namespace aircannect::FactoryReset
