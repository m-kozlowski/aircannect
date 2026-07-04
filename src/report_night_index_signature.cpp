#include "report_night_index_internal.h"

#include <algorithm>

namespace aircannect {
namespace {

constexpr uint64_t REPORT_FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t REPORT_FNV_PRIME = 1099511628211ULL;

uint64_t report_hash_bytes(uint64_t hash, const void *data, size_t len) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= REPORT_FNV_PRIME;
    }
    return hash;
}

uint64_t report_hash_u64(uint64_t hash, uint64_t value) {
    return report_hash_bytes(hash, &value, sizeof(value));
}

uint64_t report_hash_i64(uint64_t hash, int64_t value) {
    return report_hash_bytes(hash, &value, sizeof(value));
}

uint64_t report_hash_u32(uint64_t hash, uint32_t value) {
    return report_hash_bytes(hash, &value, sizeof(value));
}

uint64_t summary_identity_signature(const ReportSummaryRecord &record) {
    uint64_t hash = REPORT_FNV_OFFSET;
    hash = report_hash_u64(hash, record.start_ms);
    hash = report_hash_u64(hash, record.end_ms);
    hash = report_hash_u32(hash, record.duration_min);
    hash = report_hash_u32(hash, record.session_interval_count);

    const uint32_t session_count = std::min<uint32_t>(
        record.session_interval_count, AC_REPORT_SUMMARY_SESSION_MAX);
    for (uint32_t i = 0; i < session_count; ++i) {
        hash = report_hash_u64(hash, record.sessions[i].start_ms);
        hash = report_hash_u32(hash, record.sessions[i].duration_min);
    }

    return hash;
}

void normalize_edf_source_signatures(ReportIndexedNight &night) {
    size_t count = std::min(
        night.edf_source_signature_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));

    std::sort(night.edf_source_signatures,
              night.edf_source_signatures + count);

    size_t write = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t signature = night.edf_source_signatures[i];
        if (signature == 0) continue;
        if (write > 0 &&
            night.edf_source_signatures[write - 1] == signature) {
            continue;
        }
        night.edf_source_signatures[write++] = signature;
    }

    for (size_t i = write; i < AC_REPORT_EDF_SESSION_MAX; ++i) {
        night.edf_source_signatures[i] = 0;
    }
    night.edf_source_signature_count = write;
}

}  // namespace

uint64_t report_summary_identity_signature(
    const ReportSummaryRecord &record) {
    return summary_identity_signature(record);
}

uint64_t report_edf_session_signature(
    const EdfReportSessionDescriptor &session) {
    uint64_t hash = REPORT_FNV_OFFSET;
    hash = report_hash_bytes(hash, session.sleep_day, sizeof(session.sleep_day));
    hash = report_hash_bytes(hash,
                             session.session_stamp,
                             sizeof(session.session_stamp));
    hash = report_hash_u32(hash, session.file_mask);
    hash = report_hash_u32(hash, session.primary_signal_mask);
    hash = report_hash_u32(hash, session.fallback_signal_mask);
    hash = report_hash_u64(hash, session.total_size);
    hash = report_hash_u64(hash, static_cast<uint64_t>(session.latest_write));
    hash = report_hash_i64(hash, session.earliest_header_start_ms);
    hash = report_hash_i64(hash, session.latest_header_end_ms);
    return hash;
}

void normalize_report_indexed_night(ReportIndexedNight &night) {
    normalize_range_array(night.ranges, night.range_count);
    normalize_range_array(night.data_ranges, night.data_range_count);
    coalesce_sorted_range_array(night.data_ranges, night.data_range_count);
    normalize_edf_source_signatures(night);
}

void recompute_indexed_night_source_signature(ReportIndexedNight &night) {
    uint64_t hash = summary_identity_signature(night.summary);
    normalize_edf_source_signatures(night);

    hash = report_hash_u32(
        hash,
        static_cast<uint32_t>(night.edf_source_signature_count));
    for (size_t i = 0; i < night.edf_source_signature_count; ++i) {
        hash = report_hash_u64(hash, night.edf_source_signatures[i]);
    }

    night.source_signature = hash;
}

}  // namespace aircannect
