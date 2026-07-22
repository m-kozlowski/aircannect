#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage_stream_port.h"

namespace aircannect {

class StorageStreamService final : public StorageStreamPort {
public:
    using WakeCallback = void (*)();

    ~StorageStreamService();

    bool begin(WakeCallback wake);
    void set_task_available(bool available);
    bool step();

    bool request_stream(
        const StorageStreamCommand &command,
        std::shared_ptr<StorageByteStream> &stream_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) override;
    bool status(const StorageByteStream &stream,
                StorageStreamStatus &status_out) const override;
    bool attach(StorageByteStream &stream) override;
    StorageStreamRead read(StorageByteStream &stream,
                           uint8_t *buffer,
                           size_t max_length,
                           size_t offset) override;
    void finish(StorageByteStream &stream, bool complete) override;

private:
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    bool ready() const;
    void wake() const;

    bool open_locked(StorageByteStream &stream);
    bool produce_locked(StorageByteStream &stream);
    void fail_locked(StorageByteStream &stream, const char *error);
    void close_input_locked(StorageByteStream &stream);
    void retire_locked(size_t index, StorageStreamState state);
    size_t select_stream_locked();

    static constexpr size_t STREAM_CAPACITY = 2;

    mutable SemaphoreHandle_t lock_ = nullptr;
    std::shared_ptr<StorageByteStream> streams_[STREAM_CAPACITY];
    size_t next_stream_ = 0;
    WakeCallback wake_ = nullptr;
    bool task_available_ = false;
};

}  // namespace aircannect
