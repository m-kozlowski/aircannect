#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "report_read_plan.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

class LargeByteBuffer;

static constexpr uint8_t REPORT_NIGHT_METRIC_LEAK_MEAN_INDEX = 20;
static constexpr uint32_t REPORT_NIGHT_METRIC_LEAK_MEAN =
    1u << REPORT_NIGHT_METRIC_LEAK_MEAN_INDEX;
static constexpr uint32_t REPORT_NIGHT_METRIC_IPAP_MEAN = 1u << 21;
static constexpr uint32_t REPORT_NIGHT_METRIC_IPAP_50 = 1u << 22;
static constexpr uint32_t REPORT_NIGHT_METRIC_IPAP_95 = 1u << 23;
static_assert(static_cast<uint8_t>(NightCatalogMetric::Count) <=
                  REPORT_NIGHT_METRIC_LEAK_MEAN_INDEX,
              "catalog and derived report metrics must not overlap");

struct ReportNightMetrics {
    uint32_t valid_mask = 0;
    uint32_t str_mask = 0;
    uint32_t summary_mask = 0;
    int32_t leak_mean_milli = 0;
    int32_t ahi_milli = 0;
    int32_t obstructive_apnea_index_milli = 0;
    int32_t central_apnea_index_milli = 0;
    int32_t unknown_apnea_index_milli = 0;
    int32_t hypopnea_index_milli = 0;
    int32_t arousal_index_milli = 0;
    int32_t mask_pressure_50_milli = 0;
    int32_t leak_50_milli = 0;
    uint32_t duration_minutes = 0;
    int32_t mask_pressure_95_milli = 0;
    int32_t leak_95_milli = 0;
    int32_t minute_ventilation_50_milli = 0;
    int32_t minute_ventilation_95_milli = 0;
    int32_t respiratory_rate_50_milli = 0;
    int32_t respiratory_rate_95_milli = 0;
    int32_t tidal_volume_50_milli = 0;
    int32_t tidal_volume_95_milli = 0;
    int32_t spo2_median_milli = 0;
    uint32_t spo2_threshold_minutes = 0;
    uint32_t csr_minutes = 0;
    int32_t ipap_mean_milli = 0;
    int32_t ipap_50_milli = 0;
    int32_t ipap_95_milli = 0;
};

struct ReportEventCounts {
    uint32_t hypopnea = 0;
    uint32_t central_apnea = 0;
    uint32_t obstructive_apnea = 0;
    uint32_t unknown_apnea = 0;
    uint32_t arousal = 0;
    uint32_t csr = 0;
};

struct ReportMetricStatistics {
    bool valid = false;
    int32_t mean_milli = 0;
    int32_t p50_milli = 0;
    int32_t p95_milli = 0;
};

struct ReportCalculatedMetrics {
    ReportMetricStatistics pressure;
    ReportMetricStatistics ipap;
    ReportMetricStatistics leak;
    ReportMetricStatistics minute_ventilation;
    ReportMetricStatistics respiratory_rate;
    ReportMetricStatistics tidal_volume;
    ReportMetricStatistics spo2;
};

class ReportMetricAccumulator {
public:
    ReportMetricAccumulator();
    ~ReportMetricAccumulator();

    ReportMetricAccumulator(const ReportMetricAccumulator &) = delete;
    ReportMetricAccumulator &operator=(
        const ReportMetricAccumulator &) = delete;

    bool begin(uint32_t signal_mask);
    bool begin(const ReportReadPlan &plan);
    void accept(ReportSignalId signal, int32_t value_milli);
    ReportCalculatedMetrics finish() const;

    std::shared_ptr<const LargeByteBuffer> snapshot() const;
    bool restore(const uint8_t *data, size_t length);
    bool merge(const ReportMetricAccumulator &other);

    void clear();

private:
    struct Runtime;
    Runtime *runtime_ = nullptr;
};

bool report_signal_has_metric_consumer(ReportSignalId signal);

void report_night_count_event(ReportEventCounts &counts,
                              const ReportEventRecord &event);
void report_night_metrics_from_catalog(const NightCatalogMetrics &source,
                                       ReportNightMetrics &target);
void report_night_complete_metrics(ReportNightMetrics &metrics,
                                   const ReportEventCounts &events,
                                   const ReportCalculatedMetrics &calculated,
                                   uint8_t requested_event_mask,
                                   uint8_t missing_event_mask,
                                   uint64_t csr_duration_ms);

}  // namespace aircannect
