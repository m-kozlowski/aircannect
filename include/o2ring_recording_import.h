#pragma once

#include <optional>
#include <atomic>

#include "storage_path_port.h"
#include "storage_range_write_port.h"

namespace aircannect {

// Driven by the sensor worker. One BLE request or storage operation at a time.
class O2RingRecordingImport {
public:
    enum class Command : uint8_t { None, Open = 3, Read = 4, Close = 5, Info = 0x14 };
    static constexpr size_t BlockSize = 512;

    void begin(const char *peer, StoragePathPort &paths, StorageRangeWritePort &writes,
               StorageAtomicWritePort *clock_storage = nullptr);
    void end();
    void enable_downloads(bool enabled);
    void observe_contact(bool on_finger, bool charging, uint32_t now_ms);
    bool off_finger_timeout(uint32_t now_ms) const;
    bool needs_connection(uint32_t now_ms) const;
    void reset();
    bool active() const { return phase_ != Phase::Idle; }
    bool awaiting_list() const { return phase_ == Phase::Info; }
    Command next(uint32_t now_ms);
    const char *recording() const { return id_; }
    uint16_t block() const { return static_cast<uint16_t>(offset_ / BlockSize); }
    void reply(Command cmd, uint16_t index, const uint8_t *data, size_t size);
    void fail(const char *reason);
    void skip_open();

    // FileList and CurTIME must be durable before the sensor clock changes.
    bool save_clock(const uint8_t *info, size_t size, int64_t utc_ms, uint32_t rtt_ms);
    OperationDisposition poll_clock();
    void publish_clock_source(const char *name) { clock_source_.store(name); }

private:
    enum class Window { Unknown, Worn, OffFinger, Charging };
    static constexpr uint32_t RecordingSettleMs = 12000;
    static constexpr uint32_t WriteProgressGraceMs = 3000;
    enum class Phase { Idle, WaitForSave, WaitForRecheck, Info, Select, Open, FinalStat,
                       PartialStat, Read, Write, Close, Publish };
    bool take_list(const uint8_t *data, size_t size);
    void next_file();
    void step_path();
    void step_write(uint32_t now_ms);

    Phase phase_ = Phase::Idle;
    Window window_ = Window::Unknown;
    uint32_t idle_since_ms_ = 0, last_contact_ms_ = 0;
    bool recheck_pending_ = false;
    std::optional<uint32_t> last_write_ms_;
    StoragePathPort *paths_ = nullptr;
    StorageRangeWritePort *writes_ = nullptr;
    bool downloads_ = true;
    StorageAtomicWritePort *clock_storage_ = nullptr;
    std::atomic<const char *> clock_source_{"unknown"};
    StorageAtomicWriteCommand clock_write_;
    OperationTicket clock_ticket_;
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
