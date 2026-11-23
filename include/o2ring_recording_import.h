#pragma once

#include <optional>
#include <atomic>
#include <string_view>

#include "storage_path_port.h"
#include "storage_range_write_port.h"
#include "report_source_change.h"

namespace aircannect {

// Driven by the sensor worker. One BLE request or storage operation at a time.
class O2RingRecordingImport {
public:
    enum class Command : uint8_t { None, Open = 3, Read = 4, Close = 5, Info = 0x14 };
    enum class Addressing { Blocks, Bytes };
    static constexpr size_t BlockSize = 512;

    void begin(const char *peer, StoragePathPort &paths, StorageRangeWritePort &writes,
               StorageAtomicWritePort *clock_storage = nullptr,
               void (*published)(const ReportOximetryChange &) = nullptr);
    void end();
    void enable_downloads(bool enabled);
    void observe_contact(bool on_finger, bool charging, uint32_t now_ms);
    bool off_finger_timeout(uint32_t now_ms) const;
    bool needs_connection(uint32_t now_ms) const;
    void reset();
    bool active() const { return phase_ != Phase::Idle; }
    bool downloads_enabled() const { return downloads_; }
    bool awaiting_list() const { return phase_ == Phase::Info; }
    Command next(uint32_t now_ms);
    const char *recording() const { return id_; }
    uint16_t block() const { return static_cast<uint16_t>(offset_ / BlockSize); }
    uint32_t offset() const { return offset_; }
    void reply(Command cmd, uint16_t index, const uint8_t *data, size_t size);
    void listed(std::string_view ids);
    void opened(uint32_t size, Addressing addressing);
    void received(uint32_t offset, const uint8_t *data, size_t size);
    void fail(const char *reason);
    void skip_open(bool not_finalized = false);

    // FileList and CurTIME must be durable before the sensor clock changes.
    bool save_clock(const uint8_t *info, size_t size, int64_t utc_ms, uint32_t rtt_ms);
    bool save_clock(std::string_view list, int64_t wall_ms, int64_t utc_ms, uint32_t rtt_ms);
    void identify(std::string_view serial);
    OperationDisposition poll_clock();
    void publish_clock_source(const char *name) { clock_source_.store(name); }
    const char *serial() const { return serial_; }

private:
    enum class Window { Unknown, Worn, OffFinger, Charging };
    static constexpr uint32_t RecordingSettleMs = 12000;
    static constexpr uint32_t WriteProgressGraceMs = 3000;
    enum class Phase { Idle, WaitForSave, WaitForRecheck, Info, Select, Open, FinalStat,
                       PartialStat, Read, Write, Close, Publish };
    bool take_list(std::string_view list);
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
    void (*published_)(const ReportOximetryChange &) = nullptr;
    std::atomic<const char *> clock_source_{"unknown"};
    StorageAtomicWriteCommand clock_write_;
    OperationTicket clock_ticket_;
    OperationTicket ticket_{};
    std::shared_ptr<const LargeByteBuffer> list_, chunk_;
    size_t cursor_ = 0;
    std::optional<size_t> recheck_from_;
    std::string source_, final_, part_;
    char peer_[18]{}, serial_[33]{};
    char id_[15]{};
    uint32_t offset_ = 0, size_ = 0;
    Addressing addressing_ = Addressing::Blocks;
    bool skip_publication_ = false;
    unsigned saved_ = 0;
};

}  // namespace aircannect
