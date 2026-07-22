#include "night_catalog.h"

#include <limits>
#include <new>
#include <stdlib.h>
#include <type_traits>

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

size_t align_up(size_t value, size_t alignment) {
    if (alignment <= 1) return value;

    const size_t mask = alignment - 1;
    if (value > std::numeric_limits<size_t>::max() - mask) return 0;
    return (value + mask) & ~mask;
}

bool reserve_array(size_t &total,
                   size_t count,
                   size_t item_size,
                   size_t alignment,
                   size_t &offset) {
    offset = align_up(total, alignment);
    if (offset == 0 && total != 0) return false;
    if (count > std::numeric_limits<size_t>::max() / item_size) return false;

    const size_t bytes = count * item_size;
    if (offset > std::numeric_limits<size_t>::max() - bytes) return false;
    total = offset + bytes;
    return true;
}

void *allocate_catalog_storage(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes, false);
#else
    return calloc(1, bytes);
#endif
}

void free_catalog_storage(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

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
    free_catalog_storage(storage_);
}

bool NightCatalog::allocate(size_t record_count,
                            size_t session_count,
                            size_t mask_window_count,
                            size_t file_count,
                            size_t coverage_count,
                            size_t path_bytes) {
    size_t total = 0;
    size_t records_offset = 0;
    size_t sessions_offset = 0;
    size_t masks_offset = 0;
    size_t files_offset = 0;
    size_t coverage_offset = 0;
    size_t paths_offset = 0;

    if (!reserve_array(total,
                       record_count,
                       sizeof(NightCatalogRecord),
                       alignof(NightCatalogRecord),
                       records_offset) ||
        !reserve_array(total,
                       session_count,
                       sizeof(NightCatalogTimeRange),
                       alignof(NightCatalogTimeRange),
                       sessions_offset) ||
        !reserve_array(total,
                       mask_window_count,
                       sizeof(NightCatalogTimeRange),
                       alignof(NightCatalogTimeRange),
                       masks_offset) ||
        !reserve_array(total,
                       file_count,
                       sizeof(NightCatalogSourceFile),
                       alignof(NightCatalogSourceFile),
                       files_offset) ||
        !reserve_array(total,
                       coverage_count,
                       sizeof(NightCatalogSourceCoverage),
                       alignof(NightCatalogSourceCoverage),
                       coverage_offset) ||
        !reserve_array(total,
                       path_bytes,
                       sizeof(char),
                       alignof(char),
                       paths_offset)) {
        return false;
    }

    if (total > 0) {
        storage_ = static_cast<uint8_t *>(allocate_catalog_storage(total));
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
    paths_ = path_bytes > 0
        ? reinterpret_cast<char *>(storage_ + paths_offset)
        : nullptr;

    record_count_ = record_count;
    session_count_ = session_count;
    mask_window_count_ = mask_window_count;
    file_count_ = file_count;
    coverage_count_ = coverage_count;
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

const char *NightCatalog::path(const NightCatalogSourceFile &file) const {
    if (file.path_length == 0 || file.path_offset >= path_bytes_ ||
        file.path_length >= path_bytes_ - file.path_offset) {
        return nullptr;
    }
    return paths_ + file.path_offset;
}

}  // namespace aircannect
