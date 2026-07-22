#include "storage_stream_reader.h"

#include <limits.h>
#include <string.h>
#include <utility>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "string_util.h"

namespace aircannect {
namespace {

bool operation_running(const BackgroundOperationControl &operation,
                       char *error_out,
                       size_t error_out_size) {
    const BackgroundOperationStop stop = operation.stop_reason(millis());
    if (stop == BackgroundOperationStop::None) return true;

    copy_cstr(error_out, error_out_size,
              background_operation_stop_error(stop));
    return false;
}

void storage_wait_tick() {
    vTaskDelay(pdMS_TO_TICKS(1));
}

}  // namespace

StorageStreamReader::~StorageStreamReader() {
    close(false);
}

StorageStreamReader::StorageStreamReader(
    StorageStreamReader &&other) noexcept {
    move_from(other);
}

StorageStreamReader &StorageStreamReader::operator=(
    StorageStreamReader &&other) noexcept {
    if (this == &other) return *this;

    close(false);
    move_from(other);
    return *this;
}

void StorageStreamReader::move_from(StorageStreamReader &other) {
    port_ = other.port_;
    command_ = std::move(other.command_);
    stream_ = std::move(other.stream_);
    offset_ = other.offset_;

    other.port_ = nullptr;
    other.offset_ = 0;
}

void StorageStreamReader::configure(StorageStreamPort &port,
                                    const char *path,
                                    uint64_t expected_size,
                                    uint64_t expected_modified) {
    close(false);

    port_ = &port;
    command_ = StorageStreamCommand();
    command_.path = path ? path : "";
    command_.lane = StorageStreamLane::Export;
    command_.expected_size = expected_size;
    command_.expected_modified = expected_modified;
    command_.verify_snapshot = true;
    offset_ = 0;
}

bool StorageStreamReader::wait_for_request_slot(
    const BackgroundOperationControl &operation,
    char *error_out,
    size_t error_out_size) {
    while (operation_running(operation, error_out, error_out_size)) {
        char request_error[AC_STORAGE_ERROR_MAX] = {};
        if (port_->request_stream(command_, stream_, request_error,
                                  sizeof(request_error))) {
            return true;
        }
        if (strcmp(request_error, "stream_busy") != 0 &&
            strcmp(request_error, "stream_slots_full") != 0) {
            copy_cstr(error_out, error_out_size, request_error);
            return false;
        }

        storage_wait_tick();
    }
    return false;
}

bool StorageStreamReader::open(
    const BackgroundOperationControl &operation,
    char *error_out,
    size_t error_out_size) {
    copy_cstr(error_out, error_out_size, "");
    if (!port_ || !command_.valid()) {
        copy_cstr(error_out, error_out_size, "stream_not_configured");
        return false;
    }
    if (stream_) return true;
    if (!wait_for_request_slot(operation, error_out, error_out_size)) {
        return false;
    }

    while (operation_running(operation, error_out, error_out_size)) {
        StorageStreamStatus status;
        if (!port_->status(*stream_, status)) {
            close(false);
            copy_cstr(error_out, error_out_size, "stream_status_failed");
            return false;
        }
        if (status.state == StorageStreamState::Ready) {
            if (!port_->attach(*stream_)) {
                storage_wait_tick();
                continue;
            }
            offset_ = 0;
            return true;
        }
        if (status.state == StorageStreamState::Error ||
            status.state == StorageStreamState::Cancelled) {
            copy_cstr(error_out, error_out_size,
                      status.error[0] ? status.error : "stream_cancelled");
            close(false);
            return false;
        }

        storage_wait_tick();
    }

    close(false);
    return false;
}

bool StorageStreamReader::restart(
    const BackgroundOperationControl &operation,
    char *error_out,
    size_t error_out_size) {
    close(false);
    return open(operation, error_out, error_out_size);
}

bool StorageStreamReader::read_exact(
    uint8_t *buffer,
    size_t length,
    const BackgroundOperationControl &operation,
    char *error_out,
    size_t error_out_size) {
    copy_cstr(error_out, error_out_size, "");
    if ((!buffer && length != 0) || !stream_ || !port_) {
        copy_cstr(error_out, error_out_size, "stream_not_open");
        return false;
    }
    if (offset_ > SIZE_MAX) {
        copy_cstr(error_out, error_out_size, "stream_offset_range");
        return false;
    }

    size_t received = 0;
    while (received < length &&
           operation_running(operation, error_out, error_out_size)) {
        const StorageStreamRead read = port_->read(
            *stream_, buffer + received, length - received,
            static_cast<size_t>(offset_));
        if (read.state == StorageStreamReadState::Data) {
            received += read.bytes;
            offset_ += read.bytes;
            continue;
        }
        if (read.state == StorageStreamReadState::Retry) {
            storage_wait_tick();
            continue;
        }

        StorageStreamStatus status;
        const bool have_status = port_->status(*stream_, status);
        if (read.state == StorageStreamReadState::Error) {
            copy_cstr(error_out, error_out_size,
                      have_status && status.error[0]
                          ? status.error
                          : "stream_read_failed");
        } else {
            copy_cstr(error_out, error_out_size, "stream_read_short");
        }
        return false;
    }
    return received == length;
}

void StorageStreamReader::close(bool complete) {
    if (port_ && stream_) port_->finish(*stream_, complete);
    stream_.reset();
    offset_ = 0;
}

}  // namespace aircannect
