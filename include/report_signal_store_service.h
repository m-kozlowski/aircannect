#pragma once

#include <memory>
#include <stdint.h>

#include "report_signal_store.h"
#include "storage_atomic_write_port.h"
#include "storage_range_write_port.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportSignalStoreState : uint8_t {
    Idle,
    WritingBlock,
    PublishingEvents,
    PublishingMetadata,
    Ready,
    Failed,
    Cancelled,
};

struct ReportSignalStoreStatus {
    ReportSignalStoreState state = ReportSignalStoreState::Idle;
    SleepDayId sleep_day;
    size_t signal_index = 0;
    size_t signal_count = 0;
    uint64_t bytes_written = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const;
    bool terminal() const;
};

class ReportSignalStoreService {
public:
    static constexpr size_t MaxWriteBatchBlocks = REPORT_SIGNAL_STORE_MAX_BLOCKS;

    ReportSignalStoreService() = default;
    ~ReportSignalStoreService();

    ReportSignalStoreService(const ReportSignalStoreService &) = delete;
    ReportSignalStoreService &operator=(
        const ReportSignalStoreService &) = delete;

    void begin(StorageReadPort &read_port,
               StorageRangeWritePort &range_write_port,
               StorageAtomicWritePort &write_port);
    void begin(StorageAtomicWritePort &write_port);
    bool poll();
    void cancel();
    void reset();

    // End a complete build, not the per-track reset between block batches.
    void release_write_handles();

    // Raw buffers are shared through storage completion, including cancellation.
    // The caller must not modify them while this operation is active.
    // Track contains the accumulated bitmap, including this slot.
    // existing_block fills missing raw cells from disk. !existing_file truncates
    // stale bytes even when an abandoned attempt left a file at the same path.
    // Intermediate blocks can defer header updates; the last block of an
    // operation finalizes them before any night metadata may be published.
    OperationAdmission start_block(
        const ReportSignalStoreTrack &track,
        size_t slot,
        std::shared_ptr<LargeByteBuffer> raw,
        bool existing_block,
        bool existing_file,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane,
        bool finalize_header = true);

    // The service shares each raw block until terminal status, then resets.
    // Blocks must be new, present, and contiguous; existing blocks use
    // start_block() so their read/merge path stays single-block.
    OperationAdmission start_blocks(
        const ReportSignalStoreTrack &track,
        size_t first_slot,
        const std::shared_ptr<LargeByteBuffer> *raw_blocks,
        size_t block_count,
        bool existing_file,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane,
        bool finalize_header = true);

    // Closes signal files first; metadata is published only after success.
    OperationAdmission start(
        std::shared_ptr<ReportSignalStoreBundle> bundle,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane);

    const ReportSignalStoreStatus &status() const { return status_; }
    std::shared_ptr<const LargeByteBuffer> take_published_metadata();

private:
    enum class Phase : uint8_t {
        Idle,
        SubmitRead,
        WaitRead,
        MergeRead,
        EncodeHeader,
        SubmitHeader,
        WaitHeader,
        EncodeBlock,
        SubmitBlock,
        WaitBlock,
        SubmitFinish,
        WaitFinish,
        SubmitEvents,
        WaitEvents,
        SubmitCheckpoint,
        WaitCheckpoint,
        SubmitMetadata,
        WaitMetadata,
        Ready,
        Failed,
        Cancelled,
    };

    // Block persistence
    bool submit_read();
    bool finish_read();
    bool merge_read();
    bool encode_current();
    bool submit_range();
    bool finish_range();
    bool finish_writes();
    void advance_level();

    // Events and metadata publication
    std::shared_ptr<const LargeByteBuffer> current_bytes() const;
    bool current_path(char *path, size_t path_size) const;
    bool submit_current();
    bool finish_current();

    // Operation lifetime
    void fail(const char *error);
    void release_io();
    void clear_operation();

    StorageReadPort *read_port_ = nullptr;
    StorageRangeWritePort *range_write_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;

    // Block persistence
    ReportSignalStoreTrack track_;
    ReportSignalStoreLevel level_ = ReportSignalStoreLevel::Raw;
    ReportSignalStorePlaneRange range_;
    size_t slot_ = 0;
    int16_t *raw_ = nullptr;
    const int16_t *raw_blocks_[MaxWriteBatchBlocks] = {};
    std::shared_ptr<LargeByteBuffer> raw_buffers_[MaxWriteBatchBlocks];
    size_t block_count_ = 0;
    bool existing_file_ = false;
    bool write_header_ = true;
    std::shared_ptr<const LargeByteBuffer> block_bytes_;
    std::shared_ptr<const StorageWriteBuffers> block_buffers_;
    size_t write_size_ = 0;
    std::shared_ptr<const LargeByteBuffer> one_second_bytes_;
    OperationTicket read_ticket_;
    StoragePreparedRead prepared_;
    size_t read_offset_ = 0;
    uint8_t read_low_byte_ = 0;
    OperationTicket range_ticket_;

    // Events and metadata publication
    std::shared_ptr<ReportSignalStoreBundle> bundle_;
    std::shared_ptr<const LargeByteBuffer> published_metadata_;
    OperationTicket write_ticket_;

    // Operation lifetime
    uint32_t operation_generation_ = 0;
    StorageAtomicWriteLane lane_ = StorageAtomicWriteLane::Maintenance;
    Phase phase_ = Phase::Idle;
    ReportSignalStoreStatus status_;
};

}  // namespace aircannect
