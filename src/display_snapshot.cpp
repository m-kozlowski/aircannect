#include "display_snapshot.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "export_task.h"
#include "session_manager.h"
#include "system_status_snapshot.h"
#include "therapy_telemetry.h"

namespace aircannect {
namespace {

DisplayExportState display_state(StorageSyncRuntimeStatus status) {
    if (!status.enabled || !status.configured) {
        return DisplayExportState::Disabled;
    }
    if (status.state == StorageSyncState::Error) {
        return DisplayExportState::Error;
    }
    if (status.state == StorageSyncState::Working ||
        status.durable_state_pending) {
        return DisplayExportState::Working;
    }
    if (status.state == StorageSyncState::Pending || status.pending) {
        return DisplayExportState::Pending;
    }
    return DisplayExportState::Ready;
}

DisplayExportState display_state(SleepHqSyncRuntimeStatus status) {
    if (!status.configured) return DisplayExportState::Disabled;
    if (status.state == SleepHqSyncState::Error) {
        return DisplayExportState::Error;
    }
    if (status.state == SleepHqSyncState::Working) {
        return DisplayExportState::Working;
    }
    if (status.state == SleepHqSyncState::Pending || status.pending) {
        return DisplayExportState::Pending;
    }
    return DisplayExportState::Ready;
}

void copy_text(char *out, size_t size, const char *value) {
    if (!out || size == 0) return;
    snprintf(out, size, "%s", value ? value : "");
}

}  // namespace

DisplaySnapshot compose_display_snapshot(
    const SystemStatusSnapshot &system,
    const SessionStatus &session,
    const TherapyPressureSnapshot &pressure,
    const ExportTaskControlSnapshot &exports,
    int therapy_mode) {
    DisplaySnapshot out;

    if (system.time.esp_time_valid) {
        const time_t now = time(nullptr);
        struct tm local = {};
        if (localtime_r(&now, &local)) {
            snprintf(out.local_time, sizeof(out.local_time), "%02d:%02d",
                     local.tm_hour, local.tm_min);
        }
    }

    out.therapy_active =
        session.state == SessionState::Active ||
        system.as11.therapy_state == As11TherapyState::Running;
    if (session.state == SessionState::Active &&
        system.now_ms >= session.started_ms) {
        out.therapy_elapsed_s =
            (system.now_ms - session.started_ms) / 1000;
    }

    if (system.as11.availability == As11Availability::Unavailable ||
        !system.as11.link_connected) {
        out.air11 = DisplayAir11State::Unavailable;
    } else if (system.as11.availability == As11Availability::Available) {
        out.air11 = DisplayAir11State::Ready;
    }

    if ((system.wifi.state == "sta" || system.wifi.state == "sta_ap") &&
        strcmp(system.wifi.ip, "0.0.0.0") != 0) {
        out.wifi = DisplayWifiState::Sta;
        copy_text(out.wifi_value, sizeof(out.wifi_value), system.wifi.ip);
    } else if (system.wifi.softap_running) {
        out.wifi = DisplayWifiState::SoftAp;
        copy_text(out.wifi_value, sizeof(out.wifi_value),
                  system.wifi.softap_ssid);
    } else if (system.wifi.state == "failed" ||
               system.wifi.state == "off") {
        out.wifi = DisplayWifiState::Offline;
        copy_text(out.wifi_value, sizeof(out.wifi_value), "OFFLINE");
    } else {
        out.wifi = DisplayWifiState::Connecting;
        copy_text(out.wifi_value, sizeof(out.wifi_value), "CONNECTING");
    }

    out.smb = display_state(exports.smb);
    out.sleephq = display_state(exports.sleephq);

    out.pressure_pair = therapy_mode >= 3;
    if (out.pressure_pair && pressure.inspiratory_pressure_valid &&
        pressure.expiratory_pressure_valid) {
        out.pressure_valid = true;
        out.inspiratory_pressure = pressure.inspiratory_pressure;
        out.expiratory_pressure = pressure.expiratory_pressure;
    } else if (pressure.mask_pressure_valid) {
        out.pressure_pair = false;
        out.pressure_valid = true;
        out.pressure = pressure.mask_pressure;
    } else if (pressure.inspiratory_pressure_valid) {
        out.pressure_pair = false;
        out.pressure_valid = true;
        out.pressure = pressure.inspiratory_pressure;
    }

    out.oximetry_valid = system.oximetry.source_fresh &&
                         system.oximetry.reading.valid;
    if (out.oximetry_valid) {
        out.spo2 = system.oximetry.reading.spo2;
        out.pulse_bpm = system.oximetry.reading.pulse_bpm;
    }

    return out;
}

}  // namespace aircannect
