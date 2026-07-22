#include "ota_status.h"

#include "arduino_ota_source.h"
#include "firmware_installer.h"
#include "firmware_url_source.h"
#include "update_checker.h"

namespace aircannect {

OtaStatusSnapshot collect_ota_status(
    const FirmwareInstaller &installer,
    const FirmwareUrlSource &url_source,
    const ArduinoOtaSource &arduino_source,
    const UpdateChecker &update_checker) {
    const FirmwareInstallStatus install = installer.status();
    const FirmwareUrlSourceStatus url = url_source.status();
    const ArduinoOtaSourceStatus arduino = arduino_source.status();
    const UpdateCheckerStatus update = update_checker.status();

    OtaStatusSnapshot status;
    status.arduino_started = arduino.started;
    status.arduino_active = arduino.active;
    status.http_prepare_pending = install.prepare_pending;
    status.http_prepared = install.prepared;
    status.http_active = install.writing &&
                         install.source != FirmwareInstallSource::Arduino;
    status.http_ready = install.ready &&
                        install.source != FirmwareInstallSource::Arduino;
    status.url_active = url.active;
    status.update_check_enabled = update.enabled;
    status.update_check_pending = update.pending;
    status.update_check_active = update.active;
    status.update_check_attempted = update.attempted;
    status.update_checked = update.checked;
    status.update_available = update.available;
    status.update_installable = update.installable;
    status.update_version = update.version;
    status.update_error = update.error;
    status.update_last_check_age_ms = update.last_check_age_ms;
    status.reboot_pending = install.reboot_pending;
    status.auth_enabled = arduino.auth_enabled;
    status.arduino_port = arduino.port;

    const bool arduino_install =
        install.source == FirmwareInstallSource::Arduino;
    status.bytes = arduino_install ? arduino.bytes : install.bytes;
    status.total_size =
        arduino_install ? arduino.total_size : install.total_size;
    status.wire_bytes = arduino_install ? arduino.bytes : install.wire_bytes;
    status.wire_total_size = arduino_install
                                 ? arduino.total_size
                                 : install.wire_total_size;
    status.progress_percent = arduino_install
                                  ? arduino.progress_percent
                                  : install.progress_percent;
    if (install.source == FirmwareInstallSource::HttpUpload) {
        status.method = install.prepare_pending ? "http_prepare" : "http";
    } else {
        status.method = install.source == FirmwareInstallSource::None
                            ? "idle"
                            : firmware_install_source_name(install.source);
    }
    status.encoding = ota_upload_encoding_name(install.encoding);
    status.partition = install.partition;
    status.last_error = install.last_error;
    if (!status.last_error.length() && url.last_error.length() &&
        (url.active || install.source == FirmwareInstallSource::Url ||
         install.source == FirmwareInstallSource::None)) {
        status.last_error = url.last_error;
    }
    if (!status.last_error.length() && arduino.last_error.length() &&
        (arduino.active ||
         install.source == FirmwareInstallSource::Arduino ||
         install.source == FirmwareInstallSource::None)) {
        status.last_error = arduino.last_error;
    }
    return status;
}

bool request_selected_update(UpdateChecker &update_checker,
                             FirmwareUrlSource &url_source,
                             String &error) {
    if (update_checker.active()) {
        error = "ota_busy";
        return false;
    }

    OtaUpdateArtifact artifact;
    if (!update_checker.copy_available_artifact(artifact)) {
        error = "update_not_available";
        return false;
    }
    if (!url_source.request(artifact.url, artifact.encoding,
                            artifact.image_size, artifact.wire_size)) {
        const FirmwareUrlSourceStatus status = url_source.status();
        error = status.last_error.length() ? status.last_error : "ota_busy";
        return false;
    }

    error = "";
    return true;
}

void abort_esp_ota(FirmwareInstaller &installer,
                   FirmwareUrlSource &url_source,
                   ArduinoOtaSource &arduino_source,
                   UpdateChecker &update_checker,
                   const char *reason) {
    if (url_source.active()) {
        url_source.request_abort(reason);
        return;
    }
    if (arduino_source.active()) {
        arduino_source.request_abort(reason);
        return;
    }

    if (installer.active()) {
        installer.abort(reason);
        return;
    }

    const UpdateCheckerStatus update = update_checker.status();
    if (update.pending || update.active) {
        update_checker.request_abort(reason);
    }
}

}  // namespace aircannect
