#pragma once

#include "storage_path_port.h"
#include "storage_range_write_port.h"

namespace aircannect {

// Driven by the sensor worker. One BLE request or storage operation at a time.
class O2RingRecordingImport {
public:
    enum class Command : uint8_t { None, Open = 3, Read = 4, Close = 5, Info = 0x14 };
    static constexpr size_t BlockSize = 512;

    void begin(const char *peer, StoragePathPort &paths, StorageRangeWritePort &writes);
    void reset();
    bool active() const { return phase_ != Phase::Idle; }
    bool awaiting_list() const { return phase_ == Phase::Info; }
    Command next();
    const char *recording() const { return id_; }
    uint16_t block() const { return static_cast<uint16_t>(offset_ / BlockSize); }
    void reply(Command cmd, uint16_t index, const uint8_t *data, size_t size);
    void fail(const char *reason);
    void skip_open();

private:
    enum class Phase { Idle, Info, Select, Open, FinalStat, PartialStat, Read,
                       Write, Close, Publish };
    bool take_list(const uint8_t *data, size_t size);
    void next_file();
    void step_path();
    void step_write();

    Phase phase_ = Phase::Idle;
    StoragePathPort *paths_ = nullptr;
    StorageRangeWritePort *writes_ = nullptr;
    OperationTicket ticket_{};
    std::shared_ptr<const LargeByteBuffer> list_, chunk_;
    size_t cursor_ = 0;
    std::string source_, final_, part_;
    char id_[15]{};
    uint32_t offset_ = 0, size_ = 0;
    bool skip_publication_ = false;
    unsigned saved_ = 0;
};

}  // namespace aircannect
