#pragma once

#include <vector>
#include <string_view>
#include <utility>

#include "large_allocator.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"
#include "report_source_change.h"

namespace aircannect {

class NightCatalog;
class ReportOximetrySignalBuilder;
struct ReportOximetryPlanes;

struct ReportOximetryRecording {
    char source[33]{};
    char id[37]{};
    int64_t start_ms = 0, wall_s = 0, local_start_s = 0, observation_ms = 0;
    uint32_t samples = 0, interval_s = 0;
    uint64_t csv_bytes = 0;
    int64_t csv_end_ms = 0;
    uint64_t modified = 0;
    bool listed = false;
    char peer[18]{}, serial[33]{};
    int32_t offset_minutes = 0;
    char day[9]{};
    int64_t noon_s = 0, next_noon_s = 0;
    std::shared_ptr<const ReportOximetryPlanes> planes;

    std::string path() const;
    auto key() const { return std::make_pair(std::string_view(source), std::string_view(id)); }
    uint64_t size() const { return csv_bytes ? csv_bytes : 40ULL + 5ULL * samples; }
    int64_t end_ms() const {
        return csv_bytes ? csv_end_ms : start_ms + int64_t(samples) * interval_s * 1000;
    }
};

using ReportOximetryCatalog = std::vector<ReportOximetryRecording,
                                          LargeAllocator<ReportOximetryRecording>>;

// Resolves originals and publishes their prepared, sleep-day-scoped tracks.
class ReportOximetryArchive {
public:
    void begin(StorageScanPort &scan, StorageReadPort &read);
    void set_signal_builder(ReportOximetrySignalBuilder &builder) { signals_ = &builder; }
    void reset();
    void invalidate(const ReportOximetryChange &change) { pending_.merge(change); }
    bool poll(uint32_t now_ms, const NightCatalog *nights = nullptr);
    void night_offsets_changed() { offsets_dirty_ = true; }
    bool poll_signals(uint32_t now_ms, bool background_allowed);
    void pause_signals();
    auto ring_sources() const { return ring_sources_; }
    auto originals() const {
        return scanning_.pending || pending_.pending ? nullptr : originals_;
    }
    void withdraw(const ReportOximetryRecording &recording);
    std::shared_ptr<const ReportOximetryCatalog> catalog() const { return published_; }

private:
    void release();
    bool step(uint32_t now_ms, const NightCatalog *nights);
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
    ReportOximetrySignalBuilder *signals_ = nullptr;
    bool prepare_pending_ = false, manifest_only_ = true, offsets_dirty_ = false;
    size_t signal_index_ = 0;
    uint32_t signal_retry_at_ = 0;
    std::shared_ptr<ReportOximetryCatalog> preparing_;
    std::shared_ptr<const ReportOximetryCatalog> ring_sources_;
    std::shared_ptr<const ReportOximetryCatalog> originals_;
    ReportOximetryChange pending_{true, true}, scanning_;
    uint32_t generation_ = 0, retry_at_ = 0;
    std::shared_ptr<ReportOximetryCatalog> building_;
    std::shared_ptr<const ReportOximetryCatalog> published_;
};

}  // namespace aircannect
