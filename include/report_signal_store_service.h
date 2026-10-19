#pragma once

#include <memory>
#include <stdint.h>

#include "report_signal_store.h"
#include "report_signal_tile_writer.h"
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

struct ReportSignalTileBuildProgress {
    // RAW, 1 s, 10 s. SIZE_MAX starts at the first changed block.
    size_t next_slot[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
    size_t closed_before_slot = SIZE_MAX;
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
        bool finalize_header = true,
        const ReportSignalTileBuildProgress &tiles = {});

    // Persists RAW and its LOD levels before completing the batch. LOD is
    // computed from these buffers, without retaining data from earlier batches.
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
        bool finalize_header = true,
        const ReportSignalTileBuildProgress &tiles = {});

    // Complete only the tile suffix left pending by earlier RAW/LOD batches.
    OperationAdmission finish_tiles(
        const ReportSignalStoreTrack &track,
        const ReportSignalTileBuildProgress &tiles,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane);

    const ReportSignalTileBuildProgress &tile_progress() const { return tiles_; }

    // Closes signal files first; metadata is published only after success.
    // For partial rebuilds, every previously present sample of protected_signals
    // must still be present on the same grid in the new generation.
    OperationAdmission start(
        std::shared_ptr<ReportSignalStoreBundle> bundle,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane,
        const ReportSignalStoreMetadata &previous = {},
        uint32_t protected_signals = 0);

    // Replace only accepted metadata; signal, event and checkpoint files stay intact.
    OperationAdmission start_metadata(
        ReportSignalStoreMetadata metadata,
        uint32_t operation_generation,
        StorageAtomicWriteLane lane);

    const ReportSignalStoreStatus &status() const { return status_; }
    ReportSignalStoreMetadata take_published_metadata();

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
        PrepareTiles,
        SubmitFinish,
        WaitFinish,
        SelectPreservedBlock,
        SubmitPreservedRead,
        WaitPreservedRead,
        ComparePreservedSamples,
        SubmitEvents,
        WaitEvents,
        SubmitCheckpoint,
        WaitCheckpoint,
        SubmitHistogram,
        WaitHistogram,
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
    void prepare_tiles(StorageRangeWriteCommand memory = {}, size_t prefix = 0);

    // Events and metadata publication
    std::shared_ptr<const LargeByteBuffer> current_bytes() const;
    bool current_path(char *path, size_t path_size) const;
    bool submit_current();
    bool finish_current();

    // Partial rebuild publication guard
    struct Preservation;
    bool select_preserved_block();
    bool submit_preserved_read();
    bool finish_preserved_read();
    bool compare_preserved_samples();

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
    std::shared_ptr<const LargeByteBuffer> encoded_lod_;
    std::shared_ptr<const LargeByteBuffer> one_second_lod_;
    OperationTicket read_ticket_;
    StoragePreparedRead prepared_;
    size_t read_offset_ = 0;
    uint8_t read_low_byte_ = 0;
    OperationTicket range_ticket_;
    ReportSignalTileWriter tile_writer_;
    ReportSignalTileBuildProgress tiles_;
    bool tiles_only_ = false;

    // Events and metadata publication
    std::shared_ptr<ReportSignalStoreBundle> bundle_;
    ReportSignalStoreMetadata published_metadata_;
    OperationTicket write_ticket_;

    // Allocated in PSRAM only while checking a partial rebuild.
    Preservation *preservation_ = nullptr;

    // Operation lifetime
    uint32_t operation_generation_ = 0;
    StorageAtomicWriteLane lane_ = StorageAtomicWriteLane::Maintenance;
    Phase phase_ = Phase::Idle;
    ReportSignalStoreStatus status_;
};

}  // namespace aircannect
