#include "night_catalog_summary_snapshot.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar_utils.h"
#include "checked_size.h"
#include "large_scratch_array.h"
#include "memory_manager.h"
#include "report_daily_metrics.h"
#include "report_parser.h"

namespace aircannect {
namespace {

constexpr int64_t MS_PER_MINUTE = 60LL * 1000LL;
constexpr int64_t MS_PER_DAY = 24LL * 60LL * MS_PER_MINUTE;
constexpr int64_t LOCAL_NOON_MS = 12LL * 60LL * MS_PER_MINUTE;
constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

void set_error(char *error, size_t error_size, const char *message) {
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", message ? message : "");
}

uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

template <typename T>
uint64_t hash_value(uint64_t hash, const T &value) {
    return hash_bytes(hash, &value, sizeof(value));
}

uint64_t summary_identity(const ReportSummaryRecord &record) {
    uint64_t hash = FNV_OFFSET;
    hash = hash_value(hash, record.start_ms);
    hash = hash_value(hash, record.end_ms);
    hash = hash_value(hash, record.duration_min);
    hash = hash_value(hash, record.has_tz_offset_min);
    hash = hash_value(hash, record.tz_offset_min);
    hash = hash_value(hash, record.session_count);
    hash = hash_value(hash, record.session_interval_count);

    const size_t session_count = std::min<size_t>(
        record.session_interval_count, AC_REPORT_SUMMARY_SESSION_MAX);
    for (size_t i = 0; i < session_count; ++i) {
        hash = hash_value(hash, record.sessions[i].start_ms);
        hash = hash_value(hash, record.sessions[i].duration_min);
    }

    hash = hash_value(hash, record.summary_field_mask);
    for (size_t i = 0; i < AC_REPORT_SUMMARY_FIELD_COUNT; ++i) {
        if ((record.summary_field_mask & (1ULL << i)) == 0) continue;
        hash = hash_value(hash, record.summary_field_values[i]);
    }
    return hash == 0 ? 1 : hash;
}

bool summary_sleep_day(const ReportSummaryRecord &record, SleepDayId &out) {
    int32_t epoch_days = 0;
    return report_summary_sleep_day_epoch_days(record, epoch_days) &&
           SleepDayId::from_epoch_days(epoch_days, out);
}

bool summary_axis_timezone_offset(SleepDayId sleep_day,
                                  int64_t day_start_ms,
                                  int32_t &out) {
    if (!sleep_day.valid() || day_start_ms <= 0) return false;

    const int64_t local_noon_ms =
        static_cast<int64_t>(sleep_day.epoch_days()) * MS_PER_DAY +
        LOCAL_NOON_MS;
    const int64_t offset_ms = local_noon_ms - day_start_ms;
    if (offset_ms % MS_PER_MINUTE != 0) return false;

    const int64_t offset_minutes = offset_ms / MS_PER_MINUTE;
    if (offset_minutes < -24 * 60 || offset_minutes > 24 * 60) {
        return false;
    }

    out = static_cast<int32_t>(offset_minutes);
    return true;
}

void fill_metrics(const ReportSummaryRecord &record,
                  ReportDailyMetrics &metrics) {
    (void)report_daily_metrics_from_summary(record, metrics);
    metrics.source = ReportMetricSource::Summary;
    metrics.has_duration_min = record.duration_min > 0;
    metrics.duration_min = record.duration_min;
}

template <typename T>
void copy_summary_metric(const NightCatalogMetrics &source,
                         NightCatalogMetric metric,
                         bool &present,
                         T &value,
                         T source_value) {
    present = source.source(metric) == NightCatalogMetricSource::Summary;
    if (present) value = source_value;
}

void copy_summary_metrics(const NightCatalogMetrics &source,
                          ReportDailyMetrics &target) {
    target.source = ReportMetricSource::Summary;
    copy_summary_metric(source,
                        NightCatalogMetric::Ahi,
                        target.has_ahi,
                        target.ahi,
                        source.ahi);
    copy_summary_metric(source,
                        NightCatalogMetric::ObstructiveApneaIndex,
                        target.has_oa_index,
                        target.oa_index,
                        source.obstructive_apnea_index);
    copy_summary_metric(source,
                        NightCatalogMetric::CentralApneaIndex,
                        target.has_ca_index,
                        target.ca_index,
                        source.central_apnea_index);
    copy_summary_metric(source,
                        NightCatalogMetric::UnknownApneaIndex,
                        target.has_ua_index,
                        target.ua_index,
                        source.unknown_apnea_index);
    copy_summary_metric(source,
                        NightCatalogMetric::HypopneaIndex,
                        target.has_hypopnea_index,
                        target.hypopnea_index,
                        source.hypopnea_index);
    copy_summary_metric(source,
                        NightCatalogMetric::ArousalIndex,
                        target.has_arousal_index,
                        target.arousal_index,
                        source.arousal_index);
    copy_summary_metric(source,
                        NightCatalogMetric::MaskPressure50,
                        target.has_mask_pressure_50,
                        target.mask_pressure_50_cm_h2o,
                        source.mask_pressure_50_cm_h2o);
    copy_summary_metric(source,
                        NightCatalogMetric::MaskPressure95,
                        target.has_mask_pressure_95,
                        target.mask_pressure_95_cm_h2o,
                        source.mask_pressure_95_cm_h2o);
    copy_summary_metric(source,
                        NightCatalogMetric::Leak50,
                        target.has_leak_50,
                        target.leak_50_l_min,
                        source.leak_50_l_min);
    copy_summary_metric(source,
                        NightCatalogMetric::Leak95,
                        target.has_leak_95,
                        target.leak_95_l_min,
                        source.leak_95_l_min);
    copy_summary_metric(source,
                        NightCatalogMetric::MinuteVentilation50,
                        target.has_minute_ventilation_50,
                        target.minute_ventilation_50_l_min,
                        source.minute_ventilation_50_l_min);
    copy_summary_metric(source,
                        NightCatalogMetric::MinuteVentilation95,
                        target.has_minute_ventilation_95,
                        target.minute_ventilation_95_l_min,
                        source.minute_ventilation_95_l_min);
    copy_summary_metric(source,
                        NightCatalogMetric::RespiratoryRate50,
                        target.has_respiratory_rate_50,
                        target.respiratory_rate_50_bpm,
                        source.respiratory_rate_50_bpm);
    copy_summary_metric(source,
                        NightCatalogMetric::RespiratoryRate95,
                        target.has_respiratory_rate_95,
                        target.respiratory_rate_95_bpm,
                        source.respiratory_rate_95_bpm);
    copy_summary_metric(source,
                        NightCatalogMetric::TidalVolume50,
                        target.has_tidal_volume_50,
                        target.tidal_volume_50_l,
                        source.tidal_volume_50_l);
    copy_summary_metric(source,
                        NightCatalogMetric::TidalVolume95,
                        target.has_tidal_volume_95,
                        target.tidal_volume_95_l,
                        source.tidal_volume_95_l);
    copy_summary_metric(source,
                        NightCatalogMetric::Spo2Median,
                        target.has_spo2_50,
                        target.spo2_50_percent,
                        source.spo2_median_percent);
    copy_summary_metric(source,
                        NightCatalogMetric::Spo2ThresholdMinutes,
                        target.has_spo2_threshold_minutes,
                        target.spo2_threshold_minutes,
                        source.spo2_threshold_minutes);
    copy_summary_metric(source,
                        NightCatalogMetric::CsrMinutes,
                        target.has_csr_minutes,
                        target.csr_minutes,
                        source.csr_minutes);
    copy_summary_metric(source,
                        NightCatalogMetric::DurationMinutes,
                        target.has_duration_min,
                        target.duration_min,
                        source.duration_min);
}

bool copy_catalog_record(const NightCatalog &catalog,
                         const NightCatalogRecord &source,
                         bool expired,
                         NightCatalogSummaryInput &target,
                         NightCatalogTimeRange *sessions,
                         size_t session_capacity,
                         size_t &sessions_written) {
    size_t session_count = 0;
    const NightCatalogTimeRange *source_sessions =
        catalog.sessions(source, session_count);
    if (session_count > session_capacity ||
        (session_count > 0 && !source_sessions)) {
        return false;
    }

    target.sleep_day = source.sleep_day;
    target.day_start_ms = source.day_start_ms;
    target.day_end_ms = source.day_end_ms;
    target.sessions = session_count > 0 ? sessions : nullptr;
    target.session_count = session_count;
    target.identity = source.summary_identity;
    target.timezone_offset_minutes = source.timezone_offset_minutes;
    target.timezone_offset_valid = source.timezone_offset_valid;
    target.expired = expired;
    copy_summary_metrics(source.metrics, target.metrics);

    if (session_count > 0) {
        memcpy(sessions,
               source_sessions,
               session_count * sizeof(NightCatalogTimeRange));
    }
    sessions_written = session_count;
    return true;
}

class SummaryDayIndex {
public:
    bool build(const NightCatalogSummarySnapshot &summary) {
        if (!days_.allocate(summary.size())) return false;

        for (size_t i = 0; i < summary.size(); ++i) {
            SleepDayId *day = days_.append();
            if (!day) return false;
            *day = summary.records()[i].sleep_day;
        }
        if (days_.size() > 0) {
            std::sort(days_.data(), days_.data() + days_.size());
        }
        return true;
    }

    bool contains(SleepDayId sleep_day) const {
        if (days_.size() == 0) return false;
        return std::binary_search(days_.data(), days_.data() + days_.size(),
                                  sleep_day);
    }

private:
    LargeScratchArray<SleepDayId> days_;
};

bool expirable_summary_history(const NightCatalogRecord &record) {
    constexpr uint8_t local_sources = NIGHT_CATALOG_SOURCE_EDF |
                                      NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK;
    return (record.source_flags &
            NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK) != 0 &&
           (record.source_flags & local_sources) == 0 &&
           record.summary_identity != 0;
}

}  // namespace

bool NightCatalogSummarySnapshot::materialize_record(
    const ReportSummaryRecord &source,
    MaterializedRecord &target) {
    SleepDayId sleep_day;
    int32_t axis_timezone_offset = 0;
    if (!summary_sleep_day(source, sleep_day) ||
        source.tz_offset_min < -24 * 60 ||
        source.tz_offset_min > 24 * 60 ||
        source.start_ms > static_cast<uint64_t>(INT64_MAX) ||
        source.end_ms > static_cast<uint64_t>(INT64_MAX) ||
        source.end_ms <= source.start_ms ||
        !summary_axis_timezone_offset(
            sleep_day,
            static_cast<int64_t>(source.start_ms),
            axis_timezone_offset)) {
        return false;
    }

    target = {};
    target.sleep_day = sleep_day;
    target.day_start_ms = static_cast<int64_t>(source.start_ms);
    target.day_end_ms = static_cast<int64_t>(source.end_ms);
    target.identity = summary_identity(source);
    target.timezone_offset_minutes = axis_timezone_offset;
    fill_metrics(source, target.metrics);

    const size_t source_count = std::min<size_t>(
        source.session_interval_count, AC_REPORT_SUMMARY_SESSION_MAX);
    for (size_t i = 0; i < source_count; ++i) {
        const ReportSummarySession &session = source.sessions[i];
        if (session.start_ms == 0 || session.duration_min == 0 ||
            session.start_ms > static_cast<uint64_t>(INT64_MAX)) {
            continue;
        }

        const int64_t start_ms = static_cast<int64_t>(session.start_ms);
        const int64_t duration_ms =
            static_cast<int64_t>(session.duration_min) * MS_PER_MINUTE;
        if (start_ms > INT64_MAX - duration_ms) continue;

        target.sessions[target.session_count++] = {
            start_ms, start_ms + duration_ms};
    }
    return true;
}

bool NightCatalogSummarySnapshot::append_parsed_record(
    void *context,
    const ReportSummaryRecord &source) {
    ParseContext *parse = static_cast<ParseContext *>(context);
    if (!parse || !parse->records) return false;

    MaterializedRecord record;
    if (!materialize_record(source, record)) return true;

    try {
        parse->records->push_back(record);
    } catch (const std::bad_alloc &) {
        parse->allocation_failed = true;
        return false;
    }
    return true;
}

NightCatalogSummarySnapshot::~NightCatalogSummarySnapshot() {
    Memory::free(storage_);
}

bool NightCatalogSummarySnapshot::allocate(size_t record_count,
                                           size_t session_count) {
    size_t record_bytes = 0;
    size_t session_offset = 0;
    size_t session_bytes = 0;
    size_t total_bytes = 0;
    if (!CheckedSize::multiply(record_count,
                               sizeof(NightCatalogSummaryInput),
                               record_bytes) ||
        !CheckedSize::align_up(record_bytes,
                               alignof(NightCatalogTimeRange),
                               session_offset) ||
        !CheckedSize::multiply(session_count,
                               sizeof(NightCatalogTimeRange),
                               session_bytes) ||
        !CheckedSize::add_to(total_bytes, session_offset) ||
        !CheckedSize::add_to(total_bytes, session_bytes)) {
        return false;
    }

    if (total_bytes > 0) {
        storage_ = static_cast<uint8_t *>(
            Memory::calloc_large(1, total_bytes, false));
        if (!storage_) return false;
    }

    storage_bytes_ = total_bytes;
    records_ = reinterpret_cast<NightCatalogSummaryInput *>(storage_);
    sessions_ = reinterpret_cast<NightCatalogTimeRange *>(
        storage_ ? storage_ + session_offset : nullptr);
    record_count_ = record_count;
    session_count_ = session_count;

    for (size_t i = 0; i < record_count_; ++i) {
        new (&records_[i]) NightCatalogSummaryInput();
    }
    for (size_t i = 0; i < session_count_; ++i) {
        new (&sessions_[i]) NightCatalogTimeRange();
    }
    return true;
}

bool NightCatalogSummarySnapshot::initialize_from_materialized(
    const MaterializedRecord *records,
    size_t record_count) {
    if (record_count > 0 && !records) return false;

    size_t session_count = 0;
    for (size_t i = 0; i < record_count; ++i) {
        if (session_count > std::numeric_limits<size_t>::max() -
                                records[i].session_count) {
            return false;
        }
        session_count += records[i].session_count;
    }
    if (!allocate(record_count, session_count)) return false;

    size_t next_session = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const MaterializedRecord &source = records[i];
        NightCatalogSummaryInput &target = records_[i];
        target.sleep_day = source.sleep_day;
        target.day_start_ms = source.day_start_ms;
        target.day_end_ms = source.day_end_ms;
        target.sessions = source.session_count > 0
            ? sessions_ + next_session
            : nullptr;
        target.session_count = source.session_count;
        target.metrics = source.metrics;
        target.identity = source.identity;
        target.timezone_offset_minutes = source.timezone_offset_minutes;
        target.timezone_offset_valid = true;

        if (source.session_count > 0) {
            memcpy(sessions_ + next_session,
                   source.sessions,
                   source.session_count * sizeof(NightCatalogTimeRange));
            next_session += source.session_count;
        }
    }
    return next_session == session_count;
}

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::build(const ReportSummaryRecord *records,
                                   size_t record_count,
                                   char *error,
                                   size_t error_size) {
    if (record_count > 0 && !records) {
        set_error(error, error_size, "summary_records_missing");
        return {};
    }

    MaterializedRecords materialized;
    try {
        for (size_t i = 0; i < record_count; ++i) {
            MaterializedRecord parsed;
            if (!materialize_record(records[i], parsed)) continue;
            materialized.push_back(parsed);
        }
    } catch (const std::bad_alloc &) {
        set_error(error, error_size, "summary_snapshot_alloc_failed");
        return {};
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->initialize_from_materialized(
                         materialized.data(), materialized.size())) {
        set_error(error, error_size, "summary_snapshot_alloc_failed");
        return {};
    }

    set_error(error, error_size, "");
    return snapshot;
}

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::copy(const NightCatalogSummaryInput *records,
                                  size_t record_count) {
    if (record_count > 0 && !records) return {};

    size_t session_count = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const NightCatalogSummaryInput &record = records[i];
        if (!record.sleep_day.valid() || record.identity == 0 ||
            record.day_end_ms <= record.day_start_ms ||
            (record.timezone_offset_valid &&
             (record.timezone_offset_minutes < -24 * 60 ||
              record.timezone_offset_minutes > 24 * 60)) ||
            (record.session_count > 0 && !record.sessions) ||
            session_count > std::numeric_limits<size_t>::max() -
                                record.session_count) {
            return {};
        }
        session_count += record.session_count;
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->allocate(record_count, session_count)) {
        return {};
    }

    size_t next_session = 0;
    for (size_t i = 0; i < record_count; ++i) {
        snapshot->records_[i] = records[i];
        snapshot->records_[i].sessions = records[i].session_count > 0
            ? snapshot->sessions_ + next_session
            : nullptr;
        if (records[i].session_count > 0) {
            memcpy(snapshot->sessions_ + next_session,
                   records[i].sessions,
                   records[i].session_count *
                       sizeof(NightCatalogTimeRange));
            next_session += records[i].session_count;
        }
    }
    return snapshot;
}

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::parse(const ReportSpoolResult &result,
                                   char *error,
                                   size_t error_size) {
    MaterializedRecords materialized;
    ParseContext parse;
    parse.records = &materialized;
    if (!report_parse_summary_spool(
            result,
            NightCatalogSummarySnapshot::append_parsed_record,
            &parse,
            error,
            error_size)) {
        if (parse.allocation_failed) {
            set_error(error, error_size, "summary_snapshot_alloc_failed");
        }
        return {};
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->initialize_from_materialized(
                         materialized.data(), materialized.size())) {
        set_error(error, error_size, "summary_snapshot_alloc_failed");
        return {};
    }

    set_error(error, error_size, "");
    return snapshot;
}

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::from_catalog(const NightCatalog &catalog) {
    size_t record_count = 0;
    size_t session_count = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *record = catalog.record(i);
        if (!record ||
            (record->source_flags &
             NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK) == 0 ||
            record->summary_identity == 0) {
            continue;
        }

        size_t count = 0;
        (void)catalog.sessions(*record, count);
        if (session_count > std::numeric_limits<size_t>::max() - count) {
            return {};
        }
        ++record_count;
        session_count += count;
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->allocate(record_count, session_count)) {
        return {};
    }

    size_t next_record = 0;
    size_t next_session = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *source = catalog.record(i);
        if (!source ||
            (source->source_flags &
             NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK) == 0 ||
            source->summary_identity == 0) {
            continue;
        }

        size_t written = 0;
        const bool expired =
            (source->source_flags &
             NIGHT_CATALOG_SOURCE_SUMMARY_EXPIRED) != 0;
        NightCatalogTimeRange *session_target = next_session < session_count
            ? snapshot->sessions_ + next_session
            : nullptr;
        if (!copy_catalog_record(catalog,
                                 *source,
                                 expired,
                                 snapshot->records_[next_record],
                                 session_target,
                                 session_count - next_session,
                                 written)) {
            return {};
        }
        ++next_record;
        next_session += written;
    }

    return snapshot;
}

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::replace_night(
    const NightCatalogSummarySnapshot &current,
    const NightCatalog &single_night,
    uint64_t expected_summary_identity) {
    const NightCatalogRecord *night = single_night.size() == 1
        ? single_night.record(0) : nullptr;
    if (!night || !night->sleep_day.valid()) return {};

    size_t retained_count = 0;
    for (size_t i = 0; i < current.size(); ++i) {
        const NightCatalogSummaryInput &record = current.records()[i];
        if (record.sleep_day != night->sleep_day) {
            ++retained_count;
        } else if (record.identity != expected_summary_identity) {
            return copy(current.records(), current.size());
        }
    }

    const auto replacement = from_catalog(single_night);
    if (!replacement ||
        !CheckedSize::add_to(retained_count, replacement->size())) {
        return {};
    }

    LargeScratchArray<NightCatalogSummaryInput> records;
    if (!records.allocate(retained_count)) return {};

    bool inserted = false;
    for (size_t i = 0; i < current.size(); ++i) {
        const NightCatalogSummaryInput &record = current.records()[i];
        if (record.sleep_day != night->sleep_day) {
            *records.append() = record;
        } else if (!inserted && replacement->size() != 0) {
            *records.append() = replacement->records()[0];
            inserted = true;
        }
    }
    if (!inserted && replacement->size() != 0) {
        *records.append() = replacement->records()[0];
    }

    return copy(records.data(), records.size());
}

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::preserve_expired_history(
    const NightCatalogSummarySnapshot &current,
    const NightCatalog &previous_catalog) {
    SummaryDayIndex current_days;
    if (!current_days.build(current)) return {};

    size_t record_count = current.size();
    size_t session_count = 0;
    const NightCatalogSummaryInput *current_records = current.records();
    for (size_t i = 0; i < current.size(); ++i) {
        const NightCatalogSummaryInput &record = current_records[i];
        if (record.session_count > 0 && !record.sessions) return {};
        if (session_count > std::numeric_limits<size_t>::max() -
                                record.session_count) {
            return {};
        }
        session_count += record.session_count;
    }

    for (size_t i = 0; i < previous_catalog.size(); ++i) {
        const NightCatalogRecord *record = previous_catalog.record(i);
        if (!record || !expirable_summary_history(*record) ||
            current_days.contains(record->sleep_day)) {
            continue;
        }

        size_t count = 0;
        (void)previous_catalog.sessions(*record, count);
        if (record_count == std::numeric_limits<size_t>::max() ||
            session_count > std::numeric_limits<size_t>::max() - count) {
            return {};
        }
        ++record_count;
        session_count += count;
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->allocate(record_count, session_count)) {
        return {};
    }

    size_t next_record = 0;
    size_t next_session = 0;
    for (size_t i = 0; i < current.size(); ++i) {
        const NightCatalogSummaryInput &source = current_records[i];
        NightCatalogSummaryInput &target = snapshot->records_[next_record++];
        target = source;
        target.sessions = source.session_count > 0
            ? snapshot->sessions_ + next_session
            : nullptr;
        if (source.session_count > 0) {
            memcpy(snapshot->sessions_ + next_session,
                   source.sessions,
                   source.session_count * sizeof(NightCatalogTimeRange));
            next_session += source.session_count;
        }
    }

    for (size_t i = 0; i < previous_catalog.size(); ++i) {
        const NightCatalogRecord *source = previous_catalog.record(i);
        if (!source || !expirable_summary_history(*source) ||
            current_days.contains(source->sleep_day)) {
            continue;
        }

        size_t written = 0;
        NightCatalogTimeRange *session_target = next_session < session_count
            ? snapshot->sessions_ + next_session
            : nullptr;
        if (!copy_catalog_record(previous_catalog,
                                 *source,
                                 true,
                                 snapshot->records_[next_record],
                                 session_target,
                                 session_count - next_session,
                                 written)) {
            return {};
        }
        ++next_record;
        next_session += written;
    }

    if (next_record != record_count || next_session != session_count) {
        return {};
    }
    return snapshot;
}

}  // namespace aircannect
