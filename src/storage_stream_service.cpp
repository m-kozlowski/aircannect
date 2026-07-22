#include "storage_stream_service.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>

#include <Arduino.h>
#include <FS.h>

#include "memory_manager.h"
#include "prepared_byte_transfer.h"
#include "runtime_clock.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t STREAM_RING_BYTES = 64 * 1024;
static constexpr size_t STREAM_READ_BYTES = 16 * 1024;
static constexpr size_t STREAM_READY_BYTES = 16 * 1024;
static constexpr uint32_t STREAM_CONSUMER_TIMEOUT_MS = 60 * 1000;

uint64_t file_modified(File &file) {
    const time_t modified = file.getLastWrite();
    return modified > 0 ? static_cast<uint64_t>(modified) : 0;
}

}  // namespace

struct StorageByteStream {
    char path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
    StorageStreamLane lane = StorageStreamLane::Export;
    uint64_t expected_size = 0;
    uint64_t expected_modified = 0;
    uint64_t size = 0;
    uint64_t modified = 0;
    uint64_t produced = 0;
    uint32_t ready_ms = 0;
    StorageStreamVerification verification = StorageStreamVerification::None;
    bool metadata_ready = false;
    bool input_open = false;

    uint8_t *ring_storage = nullptr;
    PreparedByteTransfer transfer;
    File input;
    std::atomic<StorageStreamState> state{StorageStreamState::Preparing};

    ~StorageByteStream() {
        if (ring_storage) Memory::free(ring_storage);
    }
};

StorageStreamService::~StorageStreamService() {
    if (lock(20)) {
        for (std::shared_ptr<StorageByteStream> &stream : streams_) {
            if (stream) close_input_locked(*stream);
            stream.reset();
        }
        unlock();
    }
    if (lock_) vSemaphoreDelete(lock_);
}

bool StorageStreamService::begin(WakeCallback wake) {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    wake_ = wake;
    return lock_ != nullptr;
}

void StorageStreamService::set_task_available(bool available) {
    task_available_ = available;
}

bool StorageStreamService::lock(uint32_t timeout_ms) const {
    return lock_ &&
        xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageStreamService::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

bool StorageStreamService::ready() const {
    return lock_ && task_available_;
}

void StorageStreamService::wake() const {
    if (wake_) wake_();
}

bool StorageStreamService::request_stream(
    const StorageStreamCommand &command,
    std::shared_ptr<StorageByteStream> &stream_out,
    char *error_out,
    size_t error_out_size) {
    stream_out.reset();
    copy_cstr(error_out, error_out_size, "");

    if (!ready()) {
        copy_cstr(error_out, error_out_size, "service_unavailable");
        return false;
    }
    if (!command.valid() ||
        command.path.size() >= AC_STORAGE_PATH_MAX ||
        command.path.find('\0') != std::string::npos ||
        !storage_user_path_valid(command.path.c_str())) {
        copy_cstr(error_out, error_out_size, "bad_path");
        return false;
    }

    std::shared_ptr<StorageByteStream> stream(
        new (std::nothrow) StorageByteStream());
    if (!stream) {
        copy_cstr(error_out, error_out_size, "stream_alloc");
        return false;
    }

    copy_cstr(stream->path, sizeof(stream->path), command.path.c_str());
    stream->lane = command.lane;
    stream->expected_size = command.expected_size;
    stream->expected_modified = command.expected_modified;
    stream->verification = command.verification;

    if (!lock(0)) {
        copy_cstr(error_out, error_out_size, "stream_busy");
        return false;
    }

    size_t free_index = SIZE_MAX;
    for (size_t i = 0; i < STREAM_CAPACITY; ++i) {
        if (!streams_[i]) {
            free_index = i;
            break;
        }
    }
    if (free_index == SIZE_MAX) {
        unlock();
        copy_cstr(error_out, error_out_size, "stream_slots_full");
        return false;
    }

    streams_[free_index] = stream;
    stream_out = stream;
    unlock();

    wake();
    return true;
}

bool StorageStreamService::status(
    const StorageByteStream &stream,
    StorageStreamStatus &status_out) const {
    status_out = StorageStreamStatus();
    if (!ready() || !lock(20)) return false;

    status_out.state = stream.state.load(std::memory_order_acquire);
    status_out.size = stream.size;
    status_out.modified = stream.modified;
    copy_cstr(status_out.error, sizeof(status_out.error), stream.error);
    unlock();
    return true;
}

bool StorageStreamService::attach(StorageByteStream &stream) {
    if (!ready() || !lock(20)) return false;

    const bool can_attach =
        stream.state.load(std::memory_order_acquire) ==
            StorageStreamState::Ready &&
        !stream.transfer.consumer_attached() &&
        !stream.transfer.consumer_closed();
    if (can_attach) stream.transfer.attach(nonzero_millis(millis()));
    unlock();

    if (can_attach) wake();
    return can_attach;
}

StorageStreamRead StorageStreamService::read(
    StorageByteStream &stream,
    uint8_t *buffer,
    size_t max_length,
    size_t offset) {
    StorageStreamRead result;
    if (!ready()) {
        result.state = StorageStreamReadState::Error;
        return result;
    }

    const PreparedByteRead prepared = stream.transfer.read(
        buffer, max_length, offset, nonzero_millis(millis()));
    result.bytes = prepared.bytes;
    if (prepared.state == PreparedByteReadState::Data) {
        result.state = StorageStreamReadState::Data;
        wake();
        return result;
    }
    if (prepared.state == PreparedByteReadState::Retry) {
        result.state = StorageStreamReadState::Retry;
        return result;
    }

    const StorageStreamState state =
        stream.state.load(std::memory_order_acquire);
    result.state = state == StorageStreamState::Error
        ? StorageStreamReadState::Error
        : StorageStreamReadState::End;
    return result;
}

void StorageStreamService::finish(StorageByteStream &stream, bool complete) {
    if (!ready()) return;

    stream.transfer.finish(complete);
    wake();
}

void StorageStreamService::close_input_locked(StorageByteStream &stream) {
    if (!stream.input_open) return;

    Storage::Guard guard;
    stream.input.close();
    stream.input_open = false;
}

void StorageStreamService::fail_locked(StorageByteStream &stream,
                                       const char *error) {
    copy_cstr(stream.error, sizeof(stream.error), error);
    stream.ready_ms = nonzero_millis(millis());
    stream.state.store(StorageStreamState::Error,
                       std::memory_order_release);
    stream.transfer.mark_producer_done();
    close_input_locked(stream);
}

bool StorageStreamService::open_locked(StorageByteStream &stream) {
    stream.ring_storage = static_cast<uint8_t *>(
        Memory::alloc_large(STREAM_RING_BYTES, false));
    if (!stream.ring_storage) {
        fail_locked(stream, "stream_buffer_alloc");
        return false;
    }

    {
        Storage::Guard guard;
        stream.input = Storage::open(stream.path, "r");
        if (stream.input && !stream.input.isDirectory()) {
            stream.size = static_cast<uint64_t>(stream.input.size());
            stream.modified = file_modified(stream.input);
            (void)stream.input.setBufferSize(512);
            stream.input_open = true;
        } else if (stream.input) {
            stream.input.close();
        }
    }

    if (!stream.input_open) {
        fail_locked(stream, "stream_open_failed");
        return false;
    }
    const bool size_changed =
        stream.verification != StorageStreamVerification::None &&
        stream.size != stream.expected_size;
    const bool modified_changed =
        stream.verification == StorageStreamVerification::SizeAndModified &&
        stream.modified != stream.expected_modified;
    if (size_changed || modified_changed) {
        fail_locked(stream, "snapshot_changed");
        return false;
    }

    stream.transfer.bind(stream.ring_storage, STREAM_RING_BYTES);
    stream.metadata_ready = true;
    stream.ready_ms = nonzero_millis(millis());
    if (stream.size == 0) {
        stream.transfer.mark_producer_done();
        stream.state.store(StorageStreamState::Ready,
                           std::memory_order_release);
        close_input_locked(stream);
    }
    return true;
}

bool StorageStreamService::produce_locked(StorageByteStream &stream) {
    if (!stream.metadata_ready) return open_locked(stream);
    if (stream.transfer.producer_done()) return false;

    size_t span_length = 0;
    uint8_t *span = stream.transfer.write_span(span_length);
    if (!span || span_length == 0) return false;

    span_length = std::min(span_length, STREAM_READ_BYTES);
    const uint64_t remaining = stream.size - stream.produced;
    const size_t wanted = remaining < span_length
        ? static_cast<size_t>(remaining)
        : span_length;

    size_t bytes_read = 0;
    if (wanted > 0) {
        Storage::Guard guard;
        bytes_read = stream.input.read(span, wanted);
    }
    if (wanted > 0 && bytes_read != wanted) {
        fail_locked(stream, "stream_read_short");
        return true;
    }
    if (!stream.transfer.commit_write(bytes_read)) {
        fail_locked(stream, "stream_ring_commit");
        return true;
    }

    stream.produced += bytes_read;
    if (stream.produced >= stream.size) {
        uint64_t final_size = 0;
        uint64_t final_modified = 0;
        {
            Storage::Guard guard;
            final_size = static_cast<uint64_t>(stream.input.size());
            final_modified = file_modified(stream.input);
        }
        const bool size_changed =
            stream.verification != StorageStreamVerification::None &&
            final_size != stream.expected_size;
        const bool modified_changed =
            stream.verification == StorageStreamVerification::SizeAndModified &&
            final_modified != stream.expected_modified;
        if (size_changed || modified_changed) {
            fail_locked(stream, "snapshot_changed");
            return true;
        }

        stream.transfer.mark_producer_done();
        close_input_locked(stream);
    }

    const bool ready =
        stream.transfer.readable() >= STREAM_READY_BYTES ||
        stream.transfer.producer_done();
    if (ready) {
        stream.state.store(StorageStreamState::Ready,
                           std::memory_order_release);
    }
    return true;
}

void StorageStreamService::retire_locked(size_t index,
                                         StorageStreamState state) {
    if (index >= STREAM_CAPACITY || !streams_[index]) return;

    StorageByteStream &stream = *streams_[index];
    close_input_locked(stream);
    stream.state.store(state, std::memory_order_release);
    streams_[index].reset();
}

size_t StorageStreamService::select_stream_locked() {
    for (size_t offset = 0; offset < STREAM_CAPACITY; ++offset) {
        const size_t index = (next_stream_ + offset) % STREAM_CAPACITY;
        if (!streams_[index]) continue;

        next_stream_ = (index + 1) % STREAM_CAPACITY;
        return index;
    }
    return SIZE_MAX;
}

bool StorageStreamService::step() {
    if (!ready() || !lock(20)) return false;

    const size_t index = select_stream_locked();
    if (index == SIZE_MAX) {
        unlock();
        return false;
    }

    StorageByteStream &stream = *streams_[index];
    const uint32_t now_ms = nonzero_millis(millis());
    if (stream.transfer.cancel_requested()) {
        retire_locked(index, StorageStreamState::Cancelled);
        unlock();
        return true;
    }

    const StorageStreamState state =
        stream.state.load(std::memory_order_acquire);
    if (state == StorageStreamState::Error) {
        const bool expired = millis_deadline_reached(
            now_ms, stream.ready_ms + STREAM_CONSUMER_TIMEOUT_MS);
        if (stream.transfer.consumer_closed() || expired) {
            retire_locked(index, StorageStreamState::Error);
            unlock();
            return true;
        }
        unlock();
        return false;
    }

    const bool attach_expired =
        stream.metadata_ready &&
        !stream.transfer.consumer_attached() &&
        millis_deadline_reached(
            now_ms,
            stream.ready_ms + STREAM_CONSUMER_TIMEOUT_MS);
    if (attach_expired) {
        retire_locked(index, StorageStreamState::Cancelled);
        unlock();
        return true;
    }

    if (stream.transfer.producer_done()) {
        const bool complete =
            stream.transfer.consumed() == stream.size;
        if (stream.transfer.consumer_closed()) {
            retire_locked(
                index,
                complete ? StorageStreamState::Ready
                         : StorageStreamState::Cancelled);
            unlock();
            return true;
        }
        unlock();
        return false;
    }

    const bool inactive =
        stream.transfer.consumer_attached() &&
        !stream.transfer.consumer_closed() &&
        millis_deadline_reached(
            now_ms,
            stream.transfer.consumer_activity_ms() +
                STREAM_CONSUMER_TIMEOUT_MS);
    if (inactive) {
        retire_locked(index, StorageStreamState::Cancelled);
        unlock();
        return true;
    }

    const bool worked = produce_locked(stream);
    unlock();
    return worked;
}

}  // namespace aircannect
