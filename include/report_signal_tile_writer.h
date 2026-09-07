#pragma once

#include "report_signal_tile.h"
#include "storage_range_write_port.h"
#include "storage_read_port.h"

namespace aircannect {

class ReportSignalTileWriter {
public:
    ~ReportSignalTileWriter();
    void begin(StorageReadPort &read, StorageRangeWritePort &write);
    void reset();

    void start(const ReportSignalStoreTrack &track, ReportSignalStoreLevel level,
               size_t first_slot, size_t block_count,
               StorageRangeWriteCommand memory, size_t memory_prefix,
               uint32_t generation, StorageAtomicWriteLane lane);
    bool poll();

    bool active() const { return phase_ != Phase::Idle; }
    bool succeeded() const { return succeeded_; }

private:
    enum class Phase { Idle, Select, Check, Read, WaitRead, Copy, Compress,
                       Write, WaitWrite };

    void advance(bool success = true);
    void release_read();

    StorageReadPort *read_ = nullptr;
    StorageRangeWritePort *write_ = nullptr;

    Phase phase_ = Phase::Idle;
    ReportSignalStoreTrack track_;
    ReportSignalStoreLevel level_ = ReportSignalStoreLevel::Raw;
    ReportSignalTile tile_;
    size_t cursor_ = 0;
    size_t end_slot_ = 0;
    uint32_t generation_ = 0;
    StorageAtomicWriteLane lane_ = StorageAtomicWriteLane::Maintenance;
    bool succeeded_ = true;

    size_t memory_offset_ = 0;
    size_t memory_prefix_ = 0;
    StorageRangeWriteCommand memory_;
    ReportSignalStorePlaneRange memory_range_;
    bool from_memory_ = false;

    bool checking_ = false;
    OperationTicket read_ticket_;
    OperationTicket write_ticket_;
    StoragePreparedRead prepared_;
    uint64_t read_file_size_ = 0;

    std::unique_ptr<LargeByteBuffer> input_;
    std::shared_ptr<const LargeByteBuffer> output_;
    size_t copied_ = 0;
    ReportSignalTileEncoder encoder_;
};

}  // namespace aircannect
