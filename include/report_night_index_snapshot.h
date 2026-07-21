#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "report_night_index.h"

namespace aircannect {

class ReportNightIndexSnapshot {
public:
    using ReadNightFn = bool (*)(void *context,
                                 size_t index,
                                 ReportIndexedNight &out);

    static std::shared_ptr<const ReportNightIndexSnapshot> create(
        const ReportIndexedNight *nights,
        size_t count);
    static std::shared_ptr<const ReportNightIndexSnapshot> create_from_reader(
        size_t count,
        ReadNightFn reader,
        void *context);

    ~ReportNightIndexSnapshot();

    ReportNightIndexSnapshot(const ReportNightIndexSnapshot &) = delete;
    ReportNightIndexSnapshot &operator=(const ReportNightIndexSnapshot &) =
        delete;

    size_t count() const { return count_; }
    size_t therapy_night_count() const { return therapy_night_count_; }
    size_t storage_bytes() const { return storage_bytes_; }

    const ReportSummaryRecord *summary_at(size_t index) const;
    bool materialize(size_t index, ReportIndexedNight &out) const;
    bool by_therapy_index(size_t therapy_index,
                          ReportIndexedNight &out) const;
    bool by_start(uint64_t night_start_ms,
                  ReportIndexedNight &out,
                  size_t *therapy_index_out = nullptr) const;

private:
    struct Entry {
        ReportSummaryRecord summary;
        uint32_t range_offset = 0;
        uint32_t range_count = 0;
        uint32_t data_range_offset = 0;
        uint32_t data_range_count = 0;
        uint32_t signature_offset = 0;
        uint32_t signature_count = 0;
        uint64_t source_signature = 0;
        bool has_summary = false;
        bool has_edf = false;
        bool has_edf_clock_provenance = false;
        bool edf_catalog_pending = false;
    };

    ReportNightIndexSnapshot() = default;

    bool initialize(size_t count, ReadNightFn reader, void *context);
    bool visible(size_t index) const;

    uint8_t *storage_ = nullptr;
    size_t storage_bytes_ = 0;
    Entry *entries_ = nullptr;
    ReportSessionRange *ranges_ = nullptr;
    uint64_t *signatures_ = nullptr;
    size_t count_ = 0;
    size_t therapy_night_count_ = 0;
    size_t range_count_ = 0;
    size_t signature_count_ = 0;
};

using ReportNightIndexSnapshotRef =
    std::shared_ptr<const ReportNightIndexSnapshot>;

}  // namespace aircannect
