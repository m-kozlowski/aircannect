#include "night_catalog.h"

#include <new>
#include <type_traits>

#include "checked_size.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

bool metric_bit(NightCatalogMetric metric, uint16_t &bit) {
    const uint8_t index = static_cast<uint8_t>(metric);
    if (index >= static_cast<uint8_t>(NightCatalogMetric::Count) ||
        index >= 16) {
        return false;
    }

    bit = static_cast<uint16_t>(1u << index);
    return true;
}

}  // namespace

bool NightCatalogMetrics::has(NightCatalogMetric metric) const {
    uint16_t bit = 0;
    return metric_bit(metric, bit) && (valid_mask & bit) != 0;
}

NightCatalogMetricSource NightCatalogMetrics::source(
    NightCatalogMetric metric) const {
    uint16_t bit = 0;
    if (!metric_bit(metric, bit) || (valid_mask & bit) == 0) {
        return NightCatalogMetricSource::None;
    }
    if ((str_mask & bit) != 0) return NightCatalogMetricSource::Str;
    if ((summary_mask & bit) != 0) return NightCatalogMetricSource::Summary;
    return NightCatalogMetricSource::None;
}

NightCatalog::~NightCatalog() {
    Memory::free(storage_);
}

bool NightCatalog::allocate(size_t record_count,
                            size_t session_count,
                            size_t mask_window_count,
                            size_t file_count,
                            size_t coverage_count,
                            size_t signal_layout_count,
                            size_t fallback_file_count,
                            size_t fallback_section_count,
                            size_t path_bytes) {
    size_t total = 0;
    size_t records_offset = 0;
    size_t sessions_offset = 0;
    size_t masks_offset = 0;
    size_t files_offset = 0;
    size_t coverage_offset = 0;
    size_t signal_layouts_offset = 0;
    size_t fallback_files_offset = 0;
    size_t fallback_sections_offset = 0;
    size_t paths_offset = 0;

    if (!CheckedSize::reserve_array<NightCatalogRecord>(
            total, record_count, records_offset) ||
        !CheckedSize::reserve_array<NightCatalogTimeRange>(
            total, session_count, sessions_offset) ||
        !CheckedSize::reserve_array<NightCatalogTimeRange>(
            total, mask_window_count, masks_offset) ||
        !CheckedSize::reserve_array<NightCatalogSourceFile>(
            total, file_count, files_offset) ||
        !CheckedSize::reserve_array<NightCatalogSourceCoverage>(
            total, coverage_count, coverage_offset) ||
        !CheckedSize::reserve_array<EdfReportSignalLayout>(
            total, signal_layout_count, signal_layouts_offset) ||
        !CheckedSize::reserve_array<NightCatalogFallbackFile>(
            total, fallback_file_count, fallback_files_offset) ||
        !CheckedSize::reserve_array<NightCatalogFallbackSection>(
            total, fallback_section_count, fallback_sections_offset) ||
        !CheckedSize::reserve_array<char>(total, path_bytes, paths_offset)) {
        return false;
    }

    if (total > 0) {
        storage_ = static_cast<uint8_t *>(
            Memory::calloc_large(1, total, false));
        if (!storage_) return false;
    }

    storage_bytes_ = total;
    records_ = record_count > 0
        ? reinterpret_cast<NightCatalogRecord *>(storage_ + records_offset)
        : nullptr;
    sessions_ = session_count > 0
        ? reinterpret_cast<NightCatalogTimeRange *>(storage_ + sessions_offset)
        : nullptr;
    mask_windows_ = mask_window_count > 0
        ? reinterpret_cast<NightCatalogTimeRange *>(storage_ + masks_offset)
        : nullptr;
    files_ = file_count > 0
        ? reinterpret_cast<NightCatalogSourceFile *>(storage_ + files_offset)
        : nullptr;
    coverage_ = coverage_count > 0
        ? reinterpret_cast<NightCatalogSourceCoverage *>(storage_ +
                                                         coverage_offset)
        : nullptr;
    signal_layouts_ = signal_layout_count > 0
        ? reinterpret_cast<EdfReportSignalLayout *>(storage_ +
                                                    signal_layouts_offset)
        : nullptr;
    fallback_files_ = fallback_file_count > 0
        ? reinterpret_cast<NightCatalogFallbackFile *>(
              storage_ + fallback_files_offset)
        : nullptr;
    fallback_sections_ = fallback_section_count > 0
        ? reinterpret_cast<NightCatalogFallbackSection *>(
              storage_ + fallback_sections_offset)
        : nullptr;
    paths_ = path_bytes > 0
        ? reinterpret_cast<char *>(storage_ + paths_offset)
        : nullptr;

    record_count_ = record_count;
    session_count_ = session_count;
    mask_window_count_ = mask_window_count;
    file_count_ = file_count;
    coverage_count_ = coverage_count;
    signal_layout_count_ = signal_layout_count;
    fallback_file_count_ = fallback_file_count;
    fallback_section_count_ = fallback_section_count;
    path_bytes_ = path_bytes;

    static_assert(std::is_trivially_destructible<NightCatalogRecord>::value,
                  "catalog records must remain trivially destructible");
    static_assert(
        std::is_trivially_destructible<NightCatalogTimeRange>::value,
        "catalog ranges must remain trivially destructible");
    static_assert(
        std::is_trivially_destructible<NightCatalogSourceFile>::value,
        "catalog files must remain trivially destructible");
    static_assert(
        std::is_trivially_destructible<NightCatalogSourceCoverage>::value,
        "catalog coverage must remain trivially destructible");
    static_assert(
        std::is_trivially_destructible<EdfReportSignalLayout>::value,
        "catalog signal layouts must remain trivially destructible");
    static_assert(
        std::is_trivially_destructible<NightCatalogFallbackFile>::value,
        "catalog fallback files must remain trivially destructible");
    static_assert(
        std::is_trivially_destructible<NightCatalogFallbackSection>::value,
        "catalog fallback sections must remain trivially destructible");

    for (size_t i = 0; i < record_count_; ++i) {
        new (&records_[i]) NightCatalogRecord();
    }
    for (size_t i = 0; i < session_count_; ++i) {
        new (&sessions_[i]) NightCatalogTimeRange();
    }
    for (size_t i = 0; i < mask_window_count_; ++i) {
        new (&mask_windows_[i]) NightCatalogTimeRange();
    }
    for (size_t i = 0; i < file_count_; ++i) {
        new (&files_[i]) NightCatalogSourceFile();
    }
    for (size_t i = 0; i < coverage_count_; ++i) {
        new (&coverage_[i]) NightCatalogSourceCoverage();
    }
    for (size_t i = 0; i < signal_layout_count_; ++i) {
        new (&signal_layouts_[i]) EdfReportSignalLayout();
    }
    for (size_t i = 0; i < fallback_file_count_; ++i) {
        new (&fallback_files_[i]) NightCatalogFallbackFile();
    }
    for (size_t i = 0; i < fallback_section_count_; ++i) {
        new (&fallback_sections_[i]) NightCatalogFallbackSection();
    }
    return true;
}

const NightCatalogRecord *NightCatalog::record(size_t index) const {
    return index < record_count_ ? &records_[index] : nullptr;
}

const NightCatalogRecord *NightCatalog::find(SleepDayId sleep_day) const {
    size_t low = 0;
    size_t high = record_count_;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        const SleepDayId candidate = records_[mid].sleep_day;
        if (candidate == sleep_day) return &records_[mid];
        if (sleep_day < candidate) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return nullptr;
}

const NightCatalogTimeRange *NightCatalog::sessions(
    const NightCatalogRecord &record,
    size_t &count) const {
    count = record.session_count;
    if (record.session_offset > session_count_ ||
        count > session_count_ - record.session_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? sessions_ + record.session_offset : nullptr;
}

const NightCatalogTimeRange *NightCatalog::mask_windows(
    const NightCatalogRecord &record,
    size_t &count) const {
    count = record.mask_window_count;
    if (record.mask_window_offset > mask_window_count_ ||
        count > mask_window_count_ - record.mask_window_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? mask_windows_ + record.mask_window_offset : nullptr;
}

const NightCatalogSourceFile *NightCatalog::files(
    const NightCatalogRecord &record,
    size_t &count) const {
    count = record.file_count;
    if (record.file_offset > file_count_ ||
        count > file_count_ - record.file_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? files_ + record.file_offset : nullptr;
}

const NightCatalogSourceCoverage *NightCatalog::coverage(
    const NightCatalogSourceFile &file,
    size_t &count) const {
    count = file.coverage_count;
    if (file.coverage_offset > coverage_count_ ||
        count > coverage_count_ - file.coverage_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? coverage_ + file.coverage_offset : nullptr;
}

const EdfReportSignalLayout *NightCatalog::signal_layouts(
    const NightCatalogSourceFile &file,
    size_t &count) const {
    count = file.signal_layout_count;
    if (file.signal_layout_offset > signal_layout_count_ ||
        count > signal_layout_count_ - file.signal_layout_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? signal_layouts_ + file.signal_layout_offset : nullptr;
}

const char *NightCatalog::path(const NightCatalogSourceFile &file) const {
    if (file.path_length == 0 || file.path_offset >= path_bytes_ ||
        file.path_length >= path_bytes_ - file.path_offset) {
        return nullptr;
    }
    return paths_ + file.path_offset;
}

const NightCatalogFallbackFile *NightCatalog::fallback_files(
    const NightCatalogRecord &record,
    size_t &count) const {
    count = record.fallback_file_count;
    if (record.fallback_file_offset > fallback_file_count_ ||
        count > fallback_file_count_ - record.fallback_file_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0
        ? fallback_files_ + record.fallback_file_offset
        : nullptr;
}

const NightCatalogFallbackSection *NightCatalog::fallback_sections(
    const NightCatalogFallbackFile &file,
    size_t &count) const {
    count = file.section_count;
    if (file.section_offset > fallback_section_count_ ||
        count > fallback_section_count_ - file.section_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0
        ? fallback_sections_ + file.section_offset
        : nullptr;
}

const char *NightCatalog::path(
    const NightCatalogFallbackFile &file) const {
    if (file.path_length == 0 || file.path_offset >= path_bytes_ ||
        file.path_length >= path_bytes_ - file.path_offset) {
        return nullptr;
    }
    return paths_ + file.path_offset;
}

}  // namespace aircannect
