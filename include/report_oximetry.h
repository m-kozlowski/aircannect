#pragma once

#include <vector>

#include "large_allocator.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {

struct ReportOximetryRecording {
    char source[33]{};
    char id[15]{};
    int64_t wall_s = 0, local_start_s = 0, start_s = 0, observation_ms = 0;
    uint32_t samples = 0, interval_s = 0;
    uint64_t modified = 0;
    bool listed = false;
    char day[9]{};
    int64_t noon_s = 0, next_noon_s = 0;

    std::string path() const;
    uint64_t size() const { return 40ULL + 5ULL * samples; }
};

using ReportOximetryCatalog = std::vector<ReportOximetryRecording,
                                          LargeAllocator<ReportOximetryRecording>>;

// Reads archive headers and clock observations on the report worker. Waveform
// bytes remain in the immutable BIN and are streamed only for the selected day.
class ReportOximetryArchive {
public:
    void begin(StorageScanPort &scan, StorageReadPort &read);
    void reset();
    bool poll(uint32_t now_ms);
    std::shared_ptr<const ReportOximetryCatalog> catalog() const { return published_; }

private:
    void release();
    bool step(uint32_t now_ms);
    void consume(const StorageScanEntryView &entry, const uint8_t *bytes, size_t length);

    StorageScanPort *scan_ = nullptr;
    StorageReadPort *read_ = nullptr;
    OperationTicket scan_ticket_, read_ticket_;
    StoragePreparedRead prepared_;
    std::shared_ptr<const StorageScanSnapshot> files_;
    size_t index_ = 0;
    bool clocks_ = false;
    uint32_t generation_ = 0, observed_ = 0, retry_at_ = 0;
    std::shared_ptr<ReportOximetryCatalog> building_;
    std::shared_ptr<const ReportOximetryCatalog> published_;
};

}  // namespace aircannect
