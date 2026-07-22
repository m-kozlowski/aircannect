#include "night_catalog_summary_snapshot.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar_utils.h"
#include "report_daily_metrics.h"
#include "report_parser.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr int64_t MS_PER_MINUTE = 60LL * 1000LL;
constexpr int64_t MS_PER_DAY = 24LL * 60LL * MS_PER_MINUTE;
constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

void set_error(char *error, size_t error_size, const char *message) {
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", message ? message : "");
}

void *allocate_large(size_t size) {
#ifdef ARDUINO
    return Memory::calloc_large(1, size, false);
#else
    return calloc(1, size);
#endif
}

void free_large(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

bool add_size(size_t &value, size_t add) {
    if (value > std::numeric_limits<size_t>::max() - add) return false;
    value += add;
    return true;
}

bool multiply_size(size_t count, size_t width, size_t &out) {
    if (width != 0 && count > std::numeric_limits<size_t>::max() / width) {
        return false;
    }
    out = count * width;
    return true;
}

bool align_size(size_t value, size_t alignment, size_t &out) {
    if (alignment == 0) return false;

    const size_t remainder = value % alignment;
    if (remainder == 0) {
        out = value;
        return true;
    }
    if (!add_size(value, alignment - remainder)) return false;
    out = value;
    return true;
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
    if (!record.valid || record.start_ms == 0 ||
        !record.has_tz_offset_min) {
        return false;
    }

    const int64_t offset_ms =
        static_cast<int64_t>(record.tz_offset_min) * MS_PER_MINUTE;
    if (record.start_ms > static_cast<uint64_t>(INT64_MAX) ||
        (offset_ms > 0 &&
         static_cast<int64_t>(record.start_ms) > INT64_MAX - offset_ms)) {
        return false;
    }

    const int64_t local_ms =
        static_cast<int64_t>(record.start_ms) + offset_ms;
    if (local_ms < 0) return false;
    return SleepDayId::from_epoch_days(local_ms / MS_PER_DAY, out);
}

size_t valid_session_count(const ReportSummaryRecord &record) {
    const size_t count = std::min<size_t>(
        record.session_interval_count, AC_REPORT_SUMMARY_SESSION_MAX);
    size_t valid = 0;
    for (size_t i = 0; i < count; ++i) {
        const ReportSummarySession &session = record.sessions[i];
        if (session.start_ms == 0 || session.duration_min == 0 ||
            session.start_ms > static_cast<uint64_t>(INT64_MAX)) {
            continue;
        }

        const int64_t duration_ms =
            static_cast<int64_t>(session.duration_min) * MS_PER_MINUTE;
        if (static_cast<int64_t>(session.start_ms) >
            INT64_MAX - duration_ms) {
            continue;
        }
        ++valid;
    }
    return valid;
}

bool valid_record(const ReportSummaryRecord &record,
                  SleepDayId &sleep_day,
                  size_t &session_count) {
    session_count = 0;
    if (!summary_sleep_day(record, sleep_day) ||
        record.start_ms > static_cast<uint64_t>(INT64_MAX) ||
        record.end_ms > static_cast<uint64_t>(INT64_MAX) ||
        record.end_ms <= record.start_ms) {
        return false;
    }

    session_count = valid_session_count(record);
    return true;
}

void fill_metrics(const ReportSummaryRecord &record,
                  ReportDailyMetrics &metrics) {
    (void)report_daily_metrics_from_summary(record, metrics);
    metrics.source = ReportMetricSource::Summary;
    metrics.has_duration_min = record.duration_min > 0;
    metrics.duration_min = record.duration_min;
}

bool fill_record(const ReportSummaryRecord &source,
                 NightCatalogSummaryInput &target,
                 NightCatalogTimeRange *sessions,
                 size_t session_capacity,
                 size_t &sessions_written) {
    SleepDayId sleep_day;
    size_t expected_sessions = 0;
    if (!valid_record(source, sleep_day, expected_sessions) ||
        expected_sessions > session_capacity) {
        return false;
    }

    target.sleep_day = sleep_day;
    target.day_start_ms = static_cast<int64_t>(source.start_ms);
    target.day_end_ms = static_cast<int64_t>(source.end_ms);
    target.sessions = expected_sessions > 0 ? sessions : nullptr;
    target.session_count = expected_sessions;
    target.identity = summary_identity(source);
    fill_metrics(source, target.metrics);

    sessions_written = 0;
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

        sessions[sessions_written++] = {start_ms, start_ms + duration_ms};
    }
    return sessions_written == expected_sessions;
}

void copy_summary_metric(const NightCatalogMetrics &source,
                         NightCatalogMetric metric,
                         bool &present,
                         float &value,
                         float source_value) {
    present = source.source(metric) == NightCatalogMetricSource::Summary;
    if (present) value = source_value;
}

struct ParseCountContext {
    size_t records = 0;
    size_t sessions = 0;
    bool overflow = false;
};

bool count_parsed_record(void *context, const ReportSummaryRecord &record) {
    ParseCountContext *count = static_cast<ParseCountContext *>(context);
    if (!count) return false;

    SleepDayId sleep_day;
    size_t sessions = 0;
    if (!valid_record(record, sleep_day, sessions)) return true;
    if (count->records == std::numeric_limits<size_t>::max() ||
        count->sessions > std::numeric_limits<size_t>::max() - sessions) {
        count->overflow = true;
        return false;
    }

    ++count->records;
    count->sessions += sessions;
    return true;
}

struct ParseFillContext {
    NightCatalogSummaryInput *records = nullptr;
    NightCatalogTimeRange *sessions = nullptr;
    size_t record_capacity = 0;
    size_t session_capacity = 0;
    size_t record_count = 0;
    size_t session_count = 0;
};

bool fill_parsed_record(void *context, const ReportSummaryRecord &record) {
    ParseFillContext *fill = static_cast<ParseFillContext *>(context);
    if (!fill) return false;

    SleepDayId sleep_day;
    size_t expected_sessions = 0;
    if (!valid_record(record, sleep_day, expected_sessions)) return true;
    if (fill->record_count >= fill->record_capacity ||
        expected_sessions > fill->session_capacity - fill->session_count) {
        return false;
    }

    size_t written = 0;
    NightCatalogTimeRange *session_target = expected_sessions > 0
        ? fill->sessions + fill->session_count
        : nullptr;
    if (!fill_record(record,
                     fill->records[fill->record_count],
                     session_target,
                     fill->session_capacity - fill->session_count,
                     written)) {
        return false;
    }

    ++fill->record_count;
    fill->session_count += written;
    return true;
}

}  // namespace

NightCatalogSummarySnapshot::~NightCatalogSummarySnapshot() {
    free_large(storage_);
}

bool NightCatalogSummarySnapshot::allocate(size_t record_count,
                                           size_t session_count) {
    size_t record_bytes = 0;
    size_t session_offset = 0;
    size_t session_bytes = 0;
    size_t total_bytes = 0;
    if (!multiply_size(record_count,
                       sizeof(NightCatalogSummaryInput),
                       record_bytes) ||
        !align_size(record_bytes,
                    alignof(NightCatalogTimeRange),
                    session_offset) ||
        !multiply_size(session_count,
                       sizeof(NightCatalogTimeRange),
                       session_bytes) ||
        !add_size(total_bytes, session_offset) ||
        !add_size(total_bytes, session_bytes)) {
        return false;
    }

    if (total_bytes > 0) {
        storage_ = static_cast<uint8_t *>(allocate_large(total_bytes));
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

std::shared_ptr<const NightCatalogSummarySnapshot>
NightCatalogSummarySnapshot::build(const ReportSummaryRecord *records,
                                   size_t record_count,
                                   char *error,
                                   size_t error_size) {
    if (record_count > 0 && !records) {
        set_error(error, error_size, "summary_records_missing");
        return {};
    }

    ParseCountContext count;
    for (size_t i = 0; i < record_count; ++i) {
        if (!count_parsed_record(&count, records[i])) {
            set_error(error, error_size, "summary_size_overflow");
            return {};
        }
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->allocate(count.records, count.sessions)) {
        set_error(error, error_size, "summary_snapshot_alloc_failed");
        return {};
    }

    ParseFillContext fill;
    fill.records = snapshot->records_;
    fill.sessions = snapshot->sessions_;
    fill.record_capacity = count.records;
    fill.session_capacity = count.sessions;
    for (size_t i = 0; i < record_count; ++i) {
        if (!fill_parsed_record(&fill, records[i])) {
            set_error(error, error_size, "summary_snapshot_build_failed");
            return {};
        }
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
    ParseCountContext count;
    if (!report_parse_summary_spool(result,
                                    count_parsed_record,
                                    &count,
                                    error,
                                    error_size) ||
        count.overflow) {
        return {};
    }

    std::shared_ptr<NightCatalogSummarySnapshot> snapshot(
        new (std::nothrow) NightCatalogSummarySnapshot());
    if (!snapshot || !snapshot->allocate(count.records, count.sessions)) {
        set_error(error, error_size, "summary_snapshot_alloc_failed");
        return {};
    }

    ParseFillContext fill;
    fill.records = snapshot->records_;
    fill.sessions = snapshot->sessions_;
    fill.record_capacity = count.records;
    fill.session_capacity = count.sessions;
    if (!report_parse_summary_spool(result,
                                    fill_parsed_record,
                                    &fill,
                                    error,
                                    error_size) ||
        fill.record_count != count.records ||
        fill.session_count != count.sessions) {
        set_error(error, error_size, "summary_snapshot_build_failed");
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

        NightCatalogSummaryInput &target = snapshot->records_[next_record++];
        target.sleep_day = source->sleep_day;
        target.day_start_ms = source->day_start_ms;
        target.day_end_ms = source->day_end_ms;
        target.identity = source->summary_identity;

        size_t count = 0;
        const NightCatalogTimeRange *sessions =
            catalog.sessions(*source, count);
        target.sessions = count > 0
            ? snapshot->sessions_ + next_session
            : nullptr;
        target.session_count = count;
        if (count > 0) {
            memcpy(snapshot->sessions_ + next_session,
                   sessions,
                   count * sizeof(NightCatalogTimeRange));
            next_session += count;
        }

        target.metrics.source = ReportMetricSource::Summary;
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::Ahi,
                            target.metrics.has_ahi,
                            target.metrics.ahi,
                            source->metrics.ahi);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::ObstructiveApneaIndex,
                            target.metrics.has_oa_index,
                            target.metrics.oa_index,
                            source->metrics.obstructive_apnea_index);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::CentralApneaIndex,
                            target.metrics.has_ca_index,
                            target.metrics.ca_index,
                            source->metrics.central_apnea_index);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::UnknownApneaIndex,
                            target.metrics.has_ua_index,
                            target.metrics.ua_index,
                            source->metrics.unknown_apnea_index);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::HypopneaIndex,
                            target.metrics.has_hypopnea_index,
                            target.metrics.hypopnea_index,
                            source->metrics.hypopnea_index);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::ArousalIndex,
                            target.metrics.has_arousal_index,
                            target.metrics.arousal_index,
                            source->metrics.arousal_index);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::MaskPressure50,
                            target.metrics.has_mask_pressure_50,
                            target.metrics.mask_pressure_50_cm_h2o,
                            source->metrics.mask_pressure_50_cm_h2o);
        copy_summary_metric(source->metrics,
                            NightCatalogMetric::Leak50,
                            target.metrics.has_leak_50,
                            target.metrics.leak_50_l_min,
                            source->metrics.leak_50_l_min);
        if (source->metrics.source(NightCatalogMetric::DurationMinutes) ==
            NightCatalogMetricSource::Summary) {
            target.metrics.has_duration_min = true;
            target.metrics.duration_min = source->metrics.duration_min;
        }
    }

    return snapshot;
}

}  // namespace aircannect
