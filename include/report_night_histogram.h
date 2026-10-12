#pragma once

#include "report_signal_store.h"

namespace aircannect {

struct ReportNightHistogram {
    const uint8_t *bins = nullptr;
    int64_t sum_milli = 0;
    uint64_t samples = 0;
    uint32_t interval_ms = 0;
    int32_t minimum = 0;
    int32_t maximum = 0;
    uint16_t count = 0;
    bool extrema_valid = false;

    void bin(size_t entry, uint16_t &index, uint32_t &samples) const;
};

struct ReportNightHistogramView {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    uint8_t checkpoint_slot = 0;
    ReportNightHistogram metrics[ReportCalculatedMetrics::Count];

    bool matches(const ReportSignalStoreNight &night) const;
};

// Derived, sparse distributions. Views borrow the encoded buffer's lifetime.
class ReportNightHistogramCodec {
public:
    static constexpr size_t HeaderBytes = 32;
    static constexpr size_t MetricBytes = 32;
    static constexpr size_t BinBytes = 6;
    static constexpr size_t MaxBytes = HeaderBytes + ReportCalculatedMetrics::Count *
        (MetricBytes + ReportMetricAccumulator::HistogramBins * BinBytes);

    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportMetricAccumulator &metrics,
        const ReportSignalStoreNightView &night,
        ReportNightHistogramView *accepted = nullptr);
    static bool decode(const uint8_t *bytes, size_t size,
                       ReportNightHistogramView &view);
    static bool path(const ReportSignalStoreNight &night,
                     char *out, size_t size);
};

}  // namespace aircannect
