#pragma once

#include <vector>
#include <string_view>
#include <utility>

#include "large_allocator.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {

struct ReportOximetryRecording {
    char source[33]{};
    char id[37]{};
    int64_t start_ms = 0, wall_s = 0, local_start_s = 0, observation_ms = 0;
    uint32_t samples = 0, interval_s = 0;
    uint64_t csv_bytes = 0;
    int64_t csv_end_ms = 0;
    uint64_t modified = 0;
    bool listed = false;
    char day[9]{};
    int64_t noon_s = 0, next_noon_s = 0;

    std::string path() const;
    auto key() const { return std::make_pair(std::string_view(source), std::string_view(id)); }
    uint64_t size() const { return csv_bytes ? csv_bytes : 40ULL + 5ULL * samples; }
    int64_t end_ms() const {
        return csv_bytes ? csv_end_ms : start_ms + int64_t(samples) * interval_s * 1000;
    }
};

using ReportOximetryCatalog = std::vector<ReportOximetryRecording,
                                          LargeAllocator<ReportOximetryRecording>>;

// Indexes ring headers and clock observations, and the bounds of published
// live CSVs. Sample bytes stay on SD until a source is selected.
class ReportOximetryArchive {
public:
    void begin(StorageScanPort &scan, StorageReadPort &read);
    void reset();
    bool poll(uint32_t now_ms);
    bool ring_changed() const { return ring_changed_; }
    std::shared_ptr<const ReportOximetryCatalog> catalog() const { return published_; }

private:
    void release();
    bool step(uint32_t now_ms);
    void consume(const StorageScanEntryView &entry, std::string_view source, std::string_view id,
                 const uint8_t *bytes, size_t length);

    StorageScanPort *scan_ = nullptr;
    StorageReadPort *read_ = nullptr;
    OperationTicket scan_ticket_, read_ticket_;
    StoragePreparedRead prepared_;
    std::shared_ptr<const StorageScanSnapshot> files_;
    size_t index_ = 0;
    bool clocks_ = false;
    bool ring_changed_ = false;
    uint32_t ring_revision_ = 0, published_ring_revision_ = 0;
    uint32_t generation_ = 0, observed_ = 0, retry_at_ = 0;
    std::shared_ptr<ReportOximetryCatalog> building_;
    std::shared_ptr<const ReportOximetryCatalog> published_;
};

}  // namespace aircannect
