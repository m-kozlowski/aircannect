#include "resmed_firmware_preparer.h"

#include <algorithm>
#include <memory>
#include <string.h>

#include "background_operation_control.h"
#include "debug_log.h"
#include "large_byte_buffer.h"
#include "storage_path_port.h"
#include "storage_stream_port.h"
#include "storage_stream_reader.h"
#include "storage_upload_port.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t PrepareTimeoutMs = 5UL * 60UL * 1000UL;
constexpr uint32_t UploadWaitTimeoutMs = 2UL * 60UL * 1000UL;
constexpr uint32_t CleanupWaitTimeoutMs = 5000;

bool deadline_reached(uint32_t started_ms, uint32_t timeout_ms) {
    return static_cast<uint32_t>(millis() - started_ms) >= timeout_ms;
}

const char *path_filename(const char *path) {
    if (!path) return "firmware";

    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

}  // namespace

const char *resmed_firmware_prepare_state_name(
    ResmedFirmwarePrepareState state) {
    switch (state) {
        case ResmedFirmwarePrepareState::Idle: return "idle";
        case ResmedFirmwarePrepareState::Queued: return "queued";
        case ResmedFirmwarePrepareState::Inspecting: return "inspecting";
        case ResmedFirmwarePrepareState::Converting: return "converting";
        case ResmedFirmwarePrepareState::Publishing: return "publishing";
        case ResmedFirmwarePrepareState::Ready: return "ready";
        case ResmedFirmwarePrepareState::Cancelled: return "cancelled";
        case ResmedFirmwarePrepareState::Error: return "error";
    }
    return "unknown";
}

ResmedFirmwarePreparer::~ResmedFirmwarePreparer() {
    cancel_requested_.store(true, std::memory_order_release);
    if (mutex_ && !task_) vSemaphoreDelete(mutex_);
}

bool ResmedFirmwarePreparer::begin(StorageStreamPort &stream_port,
                                   StorageUploadPort &upload_port,
                                   StoragePathPort &path_port) {
    if (mutex_) return true;

    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;

    stream_port_ = &stream_port;
    upload_port_ = &upload_port;
    path_port_ = &path_port;
    return true;
}

bool ResmedFirmwarePreparer::request(const char *path,
                                     const char *filename,
                                     bool transient_source) {
    if (!path || !stream_port_ || !upload_port_ || !path_port_ ||
        !storage_user_path_valid(path) || !lock(100)) {
        return false;
    }
    if (task_ || result_pending_ || therapy_active_.load() ||
        ota_install_active_.load()) {
        unlock();
        return false;
    }

    request_ = {};
    copy_cstr(request_.path, sizeof(request_.path), path);
    copy_cstr(request_.filename, sizeof(request_.filename),
              filename && filename[0] ? filename : path_filename(path));
    copy_cstr(request_.device_identifier,
              sizeof(request_.device_identifier), device_identifier_);
    request_.transient_source = transient_source;

    result_ = {};
    status_ = {};
    status_.state = ResmedFirmwarePrepareState::Queued;
    copy_cstr(status_.source_path, sizeof(status_.source_path), path);
    copy_cstr(status_.filename, sizeof(status_.filename),
              request_.filename);
    cancel_requested_.store(false, std::memory_order_release);

    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry, "resmed_prep", AC_RESMED_PREPARE_TASK_STACK, this,
        AC_RESMED_PREPARE_TASK_PRIORITY, &task_,
        AC_RESMED_PREPARE_TASK_CORE);
    if (created != pdPASS) {
        task_ = nullptr;
        status_.state = ResmedFirmwarePrepareState::Error;
        copy_cstr(status_.error, sizeof(status_.error),
                  "prepare_task_alloc_failed");
        unlock();
        return false;
    }

    unlock();
    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] firmware preparation queued path=%s\n", path);
    return true;
}

void ResmedFirmwarePreparer::cancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

void ResmedFirmwarePreparer::publish_activity(
    const ActivitySnapshot &activity) {
    therapy_active_.store(activity.therapy_active,
                          std::memory_order_release);
    ota_install_active_.store(activity.ota_install_active,
                              std::memory_order_release);
}

void ResmedFirmwarePreparer::publish_device_identifier(
    const char *identifier) {
    if (!lock(0)) return;

    if (!strcmp(device_identifier_, identifier ? identifier : "")) {
        unlock();
        return;
    }

    copy_cstr(device_identifier_, sizeof(device_identifier_), identifier);
    unlock();
}

bool ResmedFirmwarePreparer::active() const {
    if (!lock()) return true;

    const bool result = task_ || status_.active();
    unlock();
    return result;
}

ResmedFirmwarePrepareStatus ResmedFirmwarePreparer::status() const {
    if (!lock()) return {};

    const ResmedFirmwarePrepareStatus result = status_;
    unlock();
    return result;
}

bool ResmedFirmwarePreparer::take_result(ResmedPreparedFirmware &result,
                                         bool &cancelled) {
    if (!lock()) return false;
    if (!result_pending_) {
        unlock();
        return false;
    }

    result = result_;
    cancelled = cancel_requested_.load(std::memory_order_acquire);
    result_ = {};
    result_pending_ = false;
    unlock();
    return true;
}

void ResmedFirmwarePreparer::task_entry(void *context) {
    ResmedFirmwarePreparer *preparer =
        static_cast<ResmedFirmwarePreparer *>(context);
    if (preparer) preparer->run();
    vTaskDelete(nullptr);
}

bool ResmedFirmwarePreparer::operation_should_abort(void *context) {
    const ResmedFirmwarePreparer *preparer =
        static_cast<const ResmedFirmwarePreparer *>(context);
    return !preparer || preparer->should_abort();
}

void ResmedFirmwarePreparer::run() {
    Request request;
    if (!lock(100)) {
        finish_task(ResmedFirmwarePrepareState::Error,
                    "prepare_lock_failed", nullptr);
        return;
    }
    request = request_;
    unlock();

    char error[AC_STORAGE_ERROR_MAX] = {};
    ResmedFirmwareImageInfo info;
    if (!inspect_source(request, info, error, sizeof(error))) {
        if (request.transient_source) cleanup_path(request.path);
        finish_task(should_abort()
                        ? ResmedFirmwarePrepareState::Cancelled
                        : ResmedFirmwarePrepareState::Error,
                    error[0] ? error : "image_inspection_failed", nullptr);
        return;
    }

    ResmedPreparedFirmware result;
    result.image = info;
    copy_cstr(result.source_path, sizeof(result.source_path), request.path);
    copy_cstr(result.filename, sizeof(result.filename), request.filename);
    result.cleanup_source = request.transient_source;

    if (info.kind == ResmedFirmwareImageKind::Raw) {
        if (!convert_raw(request, info, error, sizeof(error))) {
            if (request.transient_source) cleanup_path(request.path);
            cleanup_path(AC_RESMED_OTA_STAGED_PATH);
            finish_task(should_abort()
                            ? ResmedFirmwarePrepareState::Cancelled
                            : ResmedFirmwarePrepareState::Error,
                        error[0] ? error : "image_conversion_failed",
                        nullptr);
            return;
        }

        copy_cstr(result.path, sizeof(result.path),
                  AC_RESMED_OTA_STAGED_PATH);
        result.cleanup_prepared = true;
    } else {
        copy_cstr(result.path, sizeof(result.path), request.path);
    }

    finish_task(ResmedFirmwarePrepareState::Ready, nullptr, &result);
    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] firmware ready kind=%s target=%s bytes=%u path=%s\n",
              resmed_firmware_image_kind_name(info.kind), info.target,
              static_cast<unsigned>(info.prepared_size), result.path);
}

bool ResmedFirmwarePreparer::inspect_source(
    const Request &request,
    ResmedFirmwareImageInfo &info,
    char *error,
    size_t error_size) {
    publish_state(ResmedFirmwarePrepareState::Inspecting);

    StorageStreamCommand command;
    command.path = request.path;
    command.lane = StorageStreamLane::Foreground;
    command.verification = StorageStreamVerification::None;

    StorageStreamReader reader;
    reader.configure(*stream_port_, command);
    BackgroundOperationControl operation;
    operation.started_ms = millis();
    operation.timeout_ms = PrepareTimeoutMs;
    operation.should_abort = operation_should_abort;
    operation.ctx = this;
    if (!reader.open(operation, error, error_size)) return false;

    const uint64_t input_size = reader.size();
    if (input_size == 0 || input_size > AC_RESMED_OTA_MAX_FILE_BYTES) {
        copy_cstr(error, error_size, "bad_image_size");
        reader.close(false);
        return false;
    }

    ResmedFirmwareInspector inspector;
    if (!inspector.begin(input_size, request.filename,
                         request.device_identifier)) {
        copy_cstr(error, error_size, inspector.error());
        reader.close(false);
        return false;
    }

    std::unique_ptr<LargeByteBuffer> scratch =
        LargeByteBuffer::allocate(AC_RESMED_PREPARE_CHUNK_BYTES);
    if (!scratch) {
        copy_cstr(error, error_size, "prepare_buffer_alloc_failed");
        reader.close(false);
        return false;
    }

    uint64_t offset = 0;
    while (offset < input_size) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(
            scratch->size(), input_size - offset));
        if (!reader.read_exact(scratch->data(), wanted, operation,
                               error, error_size) ||
            !inspector.consume(offset, scratch->data(), wanted)) {
            if (!error[0]) copy_cstr(error, error_size, inspector.error());
            reader.close(false);
            return false;
        }

        offset += wanted;
        publish_state(ResmedFirmwarePrepareState::Inspecting,
                      input_size, offset);
    }
    reader.close(true);

    if (!inspector.finish()) {
        copy_cstr(error, error_size, inspector.error());
        return false;
    }

    info = inspector.info();
    publish_state(ResmedFirmwarePrepareState::Inspecting,
                  input_size, input_size, info.target);
    return true;
}

bool ResmedFirmwarePreparer::convert_raw(
    const Request &request,
    const ResmedFirmwareImageInfo &info,
    char *error,
    size_t error_size) {
    publish_state(ResmedFirmwarePrepareState::Converting,
                  info.prepared_size, 0, info.target);

    generation_++;
    if (generation_ == 0) generation_++;

    StorageUploadStartCommand start;
    start.path = AC_RESMED_OTA_STAGED_PATH;
    start.total_size = info.prepared_size;
    start.free_reserve_bytes = AC_RESMED_OTA_STORAGE_MARGIN_BYTES;
    start.conflict = StorageUploadConflict::Replace;
    start.generation = generation_;

    const StorageUploadStartResult started = upload_port_->start(start);
    if (!started.accepted()) {
        copy_cstr(error, error_size,
                  started.error[0] ? started.error : "upload_busy");
        return false;
    }
    const uint32_t upload_id = started.id;

    if (!wait_for_upload(upload_id, 0, false, error, error_size)) {
        (void)upload_port_->cancel(upload_id);
        return false;
    }

    uint8_t prefix[AC_RESMED_RAW_ABC_PREFIX_BYTES] = {};
    if (!resmed_build_raw_abc_prefix(info, prefix) ||
        !submit_upload_chunk(upload_id, 0, prefix, sizeof(prefix),
                             error, error_size)) {
        (void)upload_port_->cancel(upload_id);
        return false;
    }

    StorageStreamCommand command;
    command.path = request.path;
    command.lane = StorageStreamLane::Foreground;
    command.verification = StorageStreamVerification::None;

    StorageStreamReader reader;
    reader.configure(*stream_port_, command);
    BackgroundOperationControl operation;
    operation.started_ms = millis();
    operation.timeout_ms = PrepareTimeoutMs;
    operation.should_abort = operation_should_abort;
    operation.ctx = this;
    if (!reader.open(operation, error, error_size)) {
        (void)upload_port_->cancel(upload_id);
        return false;
    }

    std::unique_ptr<LargeByteBuffer> scratch =
        LargeByteBuffer::allocate(AC_RESMED_PREPARE_CHUNK_BYTES);
    if (!scratch) {
        copy_cstr(error, error_size, "prepare_buffer_alloc_failed");
        reader.close(false);
        (void)upload_port_->cancel(upload_id);
        return false;
    }

    uint64_t input_offset = 0;
    uint64_t output_offset = sizeof(prefix);
    const uint64_t payload_end = info.source_offset + info.payload_size;
    while (input_offset < info.input_size) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(
            scratch->size(), info.input_size - input_offset));
        if (!reader.read_exact(scratch->data(), wanted, operation,
                               error, error_size)) {
            reader.close(false);
            (void)upload_port_->cancel(upload_id);
            return false;
        }

        const uint64_t chunk_end = input_offset + wanted;
        const uint64_t copy_start = std::max(input_offset,
                                             info.source_offset);
        const uint64_t copy_end = std::min(chunk_end, payload_end);
        if (copy_end > copy_start) {
            const size_t data_offset =
                static_cast<size_t>(copy_start - input_offset);
            const size_t length =
                static_cast<size_t>(copy_end - copy_start);
            if (!submit_upload_chunk(upload_id, output_offset,
                                     scratch->data() + data_offset,
                                     length, error, error_size)) {
                reader.close(false);
                (void)upload_port_->cancel(upload_id);
                return false;
            }
            output_offset += length;
            publish_state(ResmedFirmwarePrepareState::Converting,
                          info.prepared_size, output_offset, info.target);
        }
        input_offset = chunk_end;
    }
    reader.close(true);

    if (output_offset != info.prepared_size) {
        copy_cstr(error, error_size, "converted_size_mismatch");
        (void)upload_port_->cancel(upload_id);
        return false;
    }

    publish_state(ResmedFirmwarePrepareState::Publishing,
                  info.prepared_size, info.prepared_size, info.target);
    return wait_for_upload(upload_id, info.prepared_size, true,
                           error, error_size);
}

bool ResmedFirmwarePreparer::submit_upload_chunk(
    uint32_t upload_id,
    uint64_t offset,
    const uint8_t *data,
    size_t length,
    char *error,
    size_t error_size) {
    if (!data || length == 0 ||
        length > AC_STORAGE_UPLOAD_CHUNK_BYTES) {
        copy_cstr(error, error_size, "invalid_output_chunk");
        return false;
    }

    std::unique_ptr<LargeByteBuffer> owned =
        LargeByteBuffer::allocate(length);
    if (!owned) {
        copy_cstr(error, error_size, "output_chunk_alloc_failed");
        return false;
    }
    memcpy(owned->data(), data, length);

    StorageUploadChunkCommand command;
    command.id = upload_id;
    command.offset = offset;
    command.bytes = LargeByteBuffer::freeze(std::move(owned));
    const StorageUploadChunkResult submitted = upload_port_->submit(command);
    if (!submitted.accepted()) {
        copy_cstr(error, error_size,
                  submitted.error[0] ? submitted.error
                                     : "output_chunk_rejected");
        return false;
    }

    return wait_for_upload(upload_id, offset + length, false,
                           error, error_size);
}

bool ResmedFirmwarePreparer::wait_for_upload(
    uint32_t upload_id,
    uint64_t committed_bytes,
    bool terminal,
    char *error,
    size_t error_size) {
    const uint32_t started_ms = millis();
    while (!should_abort() &&
           !deadline_reached(started_ms, UploadWaitTimeoutMs)) {
        StorageUploadStatus status;
        if (!upload_port_->status(upload_id, status)) {
            copy_cstr(error, error_size, "upload_status_failed");
            return false;
        }
        if (status.state == StorageUploadState::Error ||
            status.state == StorageUploadState::Cancelled) {
            copy_cstr(error, error_size,
                      status.error[0] ? status.error : "upload_cancelled");
            return false;
        }
        if (terminal && status.state == StorageUploadState::Done) return true;
        if (!terminal && status.committed_bytes >= committed_bytes &&
            (status.state == StorageUploadState::Ready ||
             status.state == StorageUploadState::Done)) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    copy_cstr(error, error_size,
              should_abort() ? "prepare_cancelled" : "upload_timeout");
    return false;
}

void ResmedFirmwarePreparer::cleanup_path(const char *path) {
    if (!path_port_ || !path || !path[0]) return;

    generation_++;
    if (generation_ == 0) generation_++;

    StoragePathCommand command;
    command.operation = StoragePathOperation::Remove;
    command.source = path;
    command.generation = generation_;
    const OperationSubmission submitted = path_port_->request(command);
    if (!submitted.accepted()) return;

    const uint32_t started_ms = millis();
    StoragePathCompletion completion;
    while (!deadline_reached(started_ms, CleanupWaitTimeoutMs)) {
        if (path_port_->take_completion(submitted.ticket, completion)) return;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    (void)path_port_->abandon(submitted.ticket);
}

bool ResmedFirmwarePreparer::should_abort() const {
    return cancel_requested_.load(std::memory_order_acquire) ||
           therapy_active_.load(std::memory_order_acquire) ||
           ota_install_active_.load(std::memory_order_acquire);
}

bool ResmedFirmwarePreparer::lock(uint32_t timeout_ms) const {
    return mutex_ &&
           xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ResmedFirmwarePreparer::unlock() const {
    if (mutex_) xSemaphoreGive(mutex_);
}

void ResmedFirmwarePreparer::publish_state(
    ResmedFirmwarePrepareState state,
    uint64_t total_bytes,
    uint64_t processed_bytes,
    const char *target,
    const char *error) {
    if (!lock()) return;

    status_.state = state;
    status_.total_bytes = total_bytes;
    status_.processed_bytes = processed_bytes;
    status_.progress_percent = total_bytes == 0
        ? 0
        : static_cast<uint8_t>(std::min<uint64_t>(
              100, (processed_bytes * 100) / total_bytes));
    if (target) copy_cstr(status_.target, sizeof(status_.target), target);
    copy_cstr(status_.error, sizeof(status_.error), error);
    unlock();
}

void ResmedFirmwarePreparer::finish_task(
    ResmedFirmwarePrepareState state,
    const char *error,
    const ResmedPreparedFirmware *result) {
    if (!lock(100)) return;

    status_.state = state;
    copy_cstr(status_.error, sizeof(status_.error), error);
    if (result) {
        result_ = *result;
        result_pending_ = true;
        status_.total_bytes = result->image.prepared_size;
        status_.processed_bytes = result->image.prepared_size;
        status_.progress_percent = 100;
        copy_cstr(status_.target, sizeof(status_.target),
                  result->image.target);
    }
    task_ = nullptr;
    unlock();
}

}  // namespace aircannect
