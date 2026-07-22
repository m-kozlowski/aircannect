#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdint.h>

#include "board.h"
#include "resmed_firmware_image.h"
#include "runtime_snapshots.h"
#include "storage_path.h"

namespace aircannect {

class StoragePathPort;
class StorageStreamPort;
class StorageUploadPort;

enum class ResmedFirmwarePrepareState : uint8_t {
    Idle,
    Queued,
    Inspecting,
    Converting,
    Publishing,
    Ready,
    Cancelled,
    Error,
};

struct ResmedPreparedFirmware {
    ResmedFirmwareImageInfo image;
    char path[AC_STORAGE_PATH_MAX] = {};
    char source_path[AC_STORAGE_PATH_MAX] = {};
    char filename[96] = {};
    bool cleanup_source = false;
    bool cleanup_prepared = false;

    bool valid() const {
        return image.valid() && path[0] != '\0' && filename[0] != '\0';
    }
};

struct ResmedFirmwarePrepareStatus {
    ResmedFirmwarePrepareState state = ResmedFirmwarePrepareState::Idle;
    uint64_t total_bytes = 0;
    uint64_t processed_bytes = 0;
    uint8_t progress_percent = 0;
    char source_path[AC_STORAGE_PATH_MAX] = {};
    char filename[96] = {};
    char target[5] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const {
        return state == ResmedFirmwarePrepareState::Queued ||
               state == ResmedFirmwarePrepareState::Inspecting ||
               state == ResmedFirmwarePrepareState::Converting ||
               state == ResmedFirmwarePrepareState::Publishing;
    }
};

const char *resmed_firmware_prepare_state_name(
    ResmedFirmwarePrepareState state);

class ResmedFirmwarePreparer {
public:
    ~ResmedFirmwarePreparer();

    bool begin(StorageStreamPort &stream_port,
               StorageUploadPort &upload_port,
               StoragePathPort &path_port);
    bool request(const char *path,
                 const char *filename,
                 bool transient_source);
    void cancel();

    void publish_activity(const ActivitySnapshot &activity);
    void publish_device_identifier(const char *identifier);

    bool active() const;
    ResmedFirmwarePrepareStatus status() const;
    bool take_result(ResmedPreparedFirmware &result, bool &cancelled);

private:
    struct Request {
        char path[AC_STORAGE_PATH_MAX] = {};
        char filename[96] = {};
        char device_identifier[96] = {};
        bool transient_source = false;
    };

    static void task_entry(void *context);
    static bool operation_should_abort(void *context);
    void run();

    bool inspect_source(const Request &request,
                        ResmedFirmwareImageInfo &info,
                        char *error,
                        size_t error_size);
    bool convert_raw(const Request &request,
                     const ResmedFirmwareImageInfo &info,
                     char *error,
                     size_t error_size);
    bool submit_upload_chunk(uint32_t upload_id,
                             uint64_t offset,
                             const uint8_t *data,
                             size_t length,
                             char *error,
                             size_t error_size);
    bool wait_for_upload(uint32_t upload_id,
                         uint64_t committed_bytes,
                         bool terminal,
                         char *error,
                         size_t error_size);
    void cleanup_path(const char *path);

    bool should_abort() const;
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void publish_state(ResmedFirmwarePrepareState state,
                       uint64_t total_bytes = 0,
                       uint64_t processed_bytes = 0,
                       const char *target = nullptr,
                       const char *error = nullptr);
    void finish_task(ResmedFirmwarePrepareState state,
                     const char *error,
                     const ResmedPreparedFirmware *result);

    StorageStreamPort *stream_port_ = nullptr;
    StorageUploadPort *upload_port_ = nullptr;
    StoragePathPort *path_port_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;

    Request request_;
    ResmedPreparedFirmware result_;
    ResmedFirmwarePrepareStatus status_;
    char device_identifier_[96] = {};
    bool result_pending_ = false;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> therapy_active_{false};
    std::atomic<bool> ota_install_active_{false};
    uint32_t generation_ = 0;
};

}  // namespace aircannect
