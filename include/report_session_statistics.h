#pragma once

#include <vector>
#include "large_allocator.h"
#include "report_signal_store.h"
#include "storage_read_port.h"

namespace aircannect {

// Contributions retain their sampling cadence; selection pools their weighted
// histograms, never their percentiles. Several contributions may share a session.
class ReportSessionStatistics {
public:
    static constexpr size_t HeaderBytes = 32;
    static constexpr size_t EntryBytes = 12;
    static constexpr size_t MaxBytes = AC_STORAGE_PREPARED_READ_MAX_BYTES;

    struct Entry {
        uint16_t session = 0;
        uint32_t interval_ms = 0;
        const uint8_t *metrics = nullptr;
        size_t size = 0;
    };

    bool accept(uint16_t session, uint32_t interval_ms, ReportSignalId signal,
                int32_t value_milli, bool expiration = false);
    std::shared_ptr<const LargeByteBuffer> finish(const ReportSignalStoreNight &night);
    void clear();

    static bool matches(const LargeByteBuffer &bytes, const ReportSignalStoreNight &night);
    static bool next(const LargeByteBuffer &bytes, size_t &offset, Entry &entry);
    static bool path(const ReportSignalStoreNight &night, char *out, size_t size);

private:
    struct Part {
        uint16_t session;
        uint32_t interval_ms;
        std::unique_ptr<ReportMetricAccumulator> metrics;
        size_t bytes = 0;
        std::shared_ptr<const LargeByteBuffer> packed;
    };

    bool account_current();
    bool pack_parts();
    std::vector<Part, LargeAllocator<Part>> parts_;
    size_t current_ = SIZE_MAX;
    size_t working_bytes_ = 0;
    size_t packed_bytes_ = HeaderBytes;
    ReportSignalId signal_ = ReportSignalId::Flow;
};

// Reconstructs a missing derived cache from published RAW planes, in bounded
// blocks. It never reads EDF, changes metadata, or starts a report rebuild.
class ReportSessionStatisticsReader {
public:
    explicit ReportSessionStatisticsReader(StorageReadPort &port);
    ~ReportSessionStatisticsReader();
    bool start(ReportSignalStoreMetadata metadata);
    bool poll();
    bool active() const;
    const char *error() const;
    std::shared_ptr<const LargeByteBuffer> take_completed();

private:
    struct Runtime;
    Runtime *runtime_;
};

}  // namespace aircannect
