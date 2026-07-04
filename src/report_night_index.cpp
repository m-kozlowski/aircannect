#include "report_night_index_internal.h"

#include <algorithm>
#include <string.h>

namespace aircannect {
namespace {

void seed_night_ranges_from_summary(ReportIndexedNight &night) {
    night.range_count = collect_session_ranges(night.summary,
                                               night.ranges,
                                               AC_REPORT_SUMMARY_SESSION_MAX);
    normalize_range_array(night.ranges, night.range_count);
}

int find_indexed_night_by_start(ReportIndexedNight *nights,
                                size_t count,
                                uint64_t start_ms) {
    if (!nights || start_ms == 0) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (nights[i].summary.valid &&
            nights[i].summary.start_ms == start_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

ReportNightIndex::ReportNightIndex(ReportIndexedNight *nights,
                                   size_t capacity)
    : nights_(nights), capacity_(capacity) {
    reset();
}

void ReportNightIndex::reset() {
    count_ = 0;
    if (nights_ && capacity_ > 0) {
        memset(nights_, 0, capacity_ * sizeof(ReportIndexedNight));
    }
}

bool ReportNightIndex::add_summary_record(
    const ReportSummaryRecord &record) {
    if (!record.valid) return true;
    if (!nights_) return false;
    int existing = find_indexed_night_by_start(nights_,
                                               count_,
                                               record.start_ms);
    if (existing < 0 && count_ >= capacity_) return false;

    ReportIndexedNight &night = existing >= 0
        ? nights_[static_cast<size_t>(existing)]
        : nights_[count_];
    const bool had_edf = night.has_edf;
    const size_t old_data_range_count = std::min(
        night.data_range_count,
        static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    ReportSessionRange old_data_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    for (size_t i = 0; i < old_data_range_count; ++i) {
        old_data_ranges[i] = night.data_ranges[i];
    }
    const size_t old_signature_count = std::min(
        night.edf_source_signature_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    uint64_t old_signatures[AC_REPORT_EDF_SESSION_MAX] = {};
    for (size_t i = 0; i < old_signature_count; ++i) {
        old_signatures[i] = night.edf_source_signatures[i];
    }

    night = ReportIndexedNight{};
    night.summary = record;
    night.has_summary = true;
    night.has_edf = had_edf;
    night.data_range_count = old_data_range_count;
    for (size_t i = 0; i < old_data_range_count; ++i) {
        night.data_ranges[i] = old_data_ranges[i];
    }
    night.edf_source_signature_count = old_signature_count;
    for (size_t i = 0; i < old_signature_count; ++i) {
        night.edf_source_signatures[i] = old_signatures[i];
    }
    night.source_signature = report_summary_identity_signature(record);
    seed_night_ranges_from_summary(night);

    if (existing < 0) count_++;
    return true;
}

bool ReportNightIndex::add_indexed_night(const ReportIndexedNight &night) {
    if (!nights_ || !night.summary.valid) return false;

    ReportIndexedNight normalized = night;
    normalize_report_indexed_night(normalized);

    const int existing = find_indexed_night_by_start(nights_,
                                                     count_,
                                                     normalized.summary.start_ms);
    if (existing >= 0) {
        nights_[static_cast<size_t>(existing)] = normalized;
        return true;
    }
    if (count_ >= capacity_) return false;

    nights_[count_++] = normalized;
    return true;
}

bool ReportNightIndex::finish(ReportIndexedNight *sort_scratch) {
    if (!nights_) return false;
    for (size_t i = 0; i < count_; ++i) {
        normalize_report_indexed_night(nights_[i]);
        recompute_indexed_night_source_signature(nights_[i]);
    }
    if (count_ < 2) return true;
    if (!sort_scratch) return false;
    for (size_t i = 1; i < count_; ++i) {
        *sort_scratch = nights_[i];
        size_t j = i;
        while (j > 0 &&
               nights_[j - 1].summary.start_ms >
                   sort_scratch->summary.start_ms) {
            nights_[j] = nights_[j - 1];
            --j;
        }
        nights_[j] = *sort_scratch;
    }
    return true;
}

bool ReportNightIndex::by_therapy_index(
    const ReportIndexedNight *nights,
    size_t count,
    size_t therapy_index,
    ReportIndexedNight &out) {
    if (!nights) return false;
    size_t current = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t index = i - 1;
        const ReportIndexedNight &night = nights[index];
        const ReportSummaryRecord &record = night.summary;
        if (!record.valid || night.range_count == 0 ||
            record.duration_min == 0) {
            continue;
        }
        if (current == therapy_index) {
            out = night;
            return true;
        }
        current++;
    }
    return false;
}

bool ReportNightIndex::by_start(const ReportIndexedNight *nights,
                                size_t count,
                                uint64_t night_start_ms,
                                ReportIndexedNight &out,
                                size_t *therapy_index_out) {
    if (!nights) return false;
    size_t current = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t index = i - 1;
        const ReportIndexedNight &night = nights[index];
        const ReportSummaryRecord &record = night.summary;
        if (!record.valid || night.range_count == 0 ||
            record.duration_min == 0) {
            continue;
        }
        if (record.start_ms == night_start_ms) {
            out = night;
            if (therapy_index_out) *therapy_index_out = current;
            return true;
        }
        current++;
    }
    return false;
}

}  // namespace aircannect
