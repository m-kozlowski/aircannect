#include "night_catalog.h"

#include <new>
#include <type_traits>

#include "checked_size.h"
#include "memory_manager.h"
#include "report_daily_metrics.h"

namespace aircannect {
namespace {

bool metric_bit(NightCatalogMetric metric, uint32_t &bit) {
    const uint8_t index = static_cast<uint8_t>(metric);
    if (index >= static_cast<uint8_t>(NightCatalogMetric::Count) ||
        index >= 32) {
        return false;
    }

    bit = 1u << index;
    return true;
}

template <typename T>
void copy_catalog_metric(const NightCatalogMetrics &source,
                         NightCatalogMetric metric,
                         NightCatalogMetricSource selected,
                         bool &present,
                         T &value,
                         T source_value) {
    present = source.source(metric) == selected;
    if (present) value = source_value;
}

}  // namespace

bool NightCatalogMetrics::has(NightCatalogMetric metric) const {
    uint32_t bit = 0;
    return metric_bit(metric, bit) && (valid_mask & bit) != 0;
}

NightCatalogMetricSource NightCatalogMetrics::source(
    NightCatalogMetric metric) const {
    uint32_t bit = 0;
    if (!metric_bit(metric, bit) || (valid_mask & bit) == 0) {
        return NightCatalogMetricSource::None;
    }
    if ((str_mask & bit) != 0) return NightCatalogMetricSource::Str;
    if ((summary_mask & bit) != 0) return NightCatalogMetricSource::Summary;
    return NightCatalogMetricSource::None;
}

void night_catalog_copy_metrics(const NightCatalogMetrics &source,
                                NightCatalogMetricSource selected,
                                ReportDailyMetrics &target) {
    target = {};
    target.source = selected == NightCatalogMetricSource::Str
        ? ReportMetricSource::StrEdf : ReportMetricSource::Summary;
    copy_catalog_metric(source,
                        NightCatalogMetric::Ahi, selected,
                        target.has_ahi,
                        target.ahi,
                        source.ahi);
    copy_catalog_metric(source,
                        NightCatalogMetric::ObstructiveApneaIndex, selected,
                        target.has_oa_index,
                        target.oa_index,
                        source.obstructive_apnea_index);
    copy_catalog_metric(source,
                        NightCatalogMetric::CentralApneaIndex, selected,
                        target.has_ca_index,
                        target.ca_index,
                        source.central_apnea_index);
    copy_catalog_metric(source,
                        NightCatalogMetric::UnknownApneaIndex, selected,
                        target.has_ua_index,
                        target.ua_index,
                        source.unknown_apnea_index);
    copy_catalog_metric(source,
                        NightCatalogMetric::HypopneaIndex, selected,
                        target.has_hypopnea_index,
                        target.hypopnea_index,
                        source.hypopnea_index);
    copy_catalog_metric(source,
                        NightCatalogMetric::ArousalIndex, selected,
                        target.has_arousal_index,
                        target.arousal_index,
                        source.arousal_index);
    copy_catalog_metric(source,
                        NightCatalogMetric::MaskPressure50, selected,
                        target.has_mask_pressure_50,
                        target.mask_pressure_50_cm_h2o,
                        source.mask_pressure_50_cm_h2o);
    copy_catalog_metric(source,
                        NightCatalogMetric::MaskPressure95, selected,
                        target.has_mask_pressure_95,
                        target.mask_pressure_95_cm_h2o,
                        source.mask_pressure_95_cm_h2o);
    copy_catalog_metric(source,
                        NightCatalogMetric::Leak50, selected,
                        target.has_leak_50,
                        target.leak_50_l_min,
                        source.leak_50_l_min);
    copy_catalog_metric(source,
                        NightCatalogMetric::Leak95, selected,
                        target.has_leak_95,
                        target.leak_95_l_min,
                        source.leak_95_l_min);
    copy_catalog_metric(source,
                        NightCatalogMetric::MinuteVentilation50, selected,
                        target.has_minute_ventilation_50,
                        target.minute_ventilation_50_l_min,
                        source.minute_ventilation_50_l_min);
    copy_catalog_metric(source,
                        NightCatalogMetric::MinuteVentilation95, selected,
                        target.has_minute_ventilation_95,
                        target.minute_ventilation_95_l_min,
                        source.minute_ventilation_95_l_min);
    copy_catalog_metric(source,
                        NightCatalogMetric::RespiratoryRate50, selected,
                        target.has_respiratory_rate_50,
                        target.respiratory_rate_50_bpm,
                        source.respiratory_rate_50_bpm);
    copy_catalog_metric(source,
                        NightCatalogMetric::RespiratoryRate95, selected,
                        target.has_respiratory_rate_95,
                        target.respiratory_rate_95_bpm,
                        source.respiratory_rate_95_bpm);
    copy_catalog_metric(source,
                        NightCatalogMetric::TidalVolume50, selected,
                        target.has_tidal_volume_50,
                        target.tidal_volume_50_l,
                        source.tidal_volume_50_l);
    copy_catalog_metric(source,
                        NightCatalogMetric::TidalVolume95, selected,
                        target.has_tidal_volume_95,
                        target.tidal_volume_95_l,
                        source.tidal_volume_95_l);
    copy_catalog_metric(source,
                        NightCatalogMetric::Spo2Median, selected,
                        target.has_spo2_50,
                        target.spo2_50_percent,
                        source.spo2_median_percent);
    copy_catalog_metric(source,
                        NightCatalogMetric::Spo2ThresholdMinutes, selected,
                        target.has_spo2_threshold_minutes,
                        target.spo2_threshold_minutes,
                        source.spo2_threshold_minutes);
    copy_catalog_metric(source,
                        NightCatalogMetric::CsrMinutes, selected,
                        target.has_csr_minutes,
                        target.csr_minutes,
                        source.csr_minutes);
    copy_catalog_metric(source,
                        NightCatalogMetric::DurationMinutes, selected,
                        target.has_duration_min,
                        target.duration_min,
                        source.duration_min);
}

NightCatalog::~NightCatalog() {
    Memory::free(storage_);
}

std::shared_ptr<NightCatalog> NightCatalog::create() try {
    // Keep construction private while allocating the owner and control block together.
    struct SharedCatalog : NightCatalog {};
    return std::allocate_shared<SharedCatalog>(LargeAllocator<SharedCatalog>());
} catch (const std::bad_alloc &) {
    return {};
}

void NightCatalog::summarize() {
    time_conflict_nights_ = 0;
    unresolved_history_nights_ = 0;
    if (!blocks_.empty()) {
        for (const auto &block : blocks_) {
            time_conflict_nights_ += block.owner->time_conflict_nights();
            unresolved_history_nights_ += block.owner->unresolved_history_nights();
        }
        return;
    }
    for (size_t i = 0; i < size(); ++i) {
        const auto &night = *record(i);
        if (night.source_flags & NIGHT_CATALOG_SOURCE_TIME_CONFLICT) {
            ++time_conflict_nights_;
        }
        if (night.unresolved_history) ++unresolved_history_nights_;
    }
}

bool NightCatalog::append_block(std::shared_ptr<const NightCatalog> block) try {
    if (!block || block->size() == 0) return true;

    blocks_.push_back({block, record_count_});
    record_count_ += block->record_count_;
    session_count_ += block->session_count_;
    mask_window_count_ += block->mask_window_count_;
    file_count_ += block->file_count_;
    coverage_count_ += block->coverage_count_;
    signal_layout_count_ += block->signal_layout_count_;
    fallback_file_count_ += block->fallback_file_count_;
    fallback_section_count_ += block->fallback_section_count_;
    path_bytes_ += block->path_bytes_;
    storage_bytes_ += block->storage_bytes();
    time_conflict_nights_ += block->time_conflict_nights();
    unresolved_history_nights_ += block->unresolved_history_nights();
    return true;
} catch (const std::bad_alloc &) {
    return false;
}

const NightCatalog *NightCatalog::record_owner(SleepDayId day) const {
    if (blocks_.empty()) return this;

    size_t low = 0;
    size_t high = blocks_.size();
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        const auto &block = *blocks_[mid].owner;
        if (day < block.record(block.size() - 1)->sleep_day) low = mid + 1;
        else high = mid;
    }
    return low < blocks_.size() ? blocks_[low].owner.get() : nullptr;
}

template <typename File>
const NightCatalog *NightCatalog::file_owner(const File &file) const {
    if (blocks_.empty()) return this;

    const uintptr_t address = reinterpret_cast<uintptr_t>(&file);
    for (const auto &block : blocks_) {
        const auto &owner = *block.owner;
        const File *files;
        size_t count;
        if constexpr (std::is_same<File, NightCatalogSourceFile>::value) {
            files = owner.files_;
            count = owner.file_count_;
        } else {
            files = owner.fallback_files_;
            count = owner.fallback_file_count_;
        }

        const uintptr_t first = reinterpret_cast<uintptr_t>(files);
        if (address >= first && address - first < count * sizeof(file)) {
            return &owner;
        }
    }
    return nullptr;
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
    if (!blocks_.empty()) {
        if (index >= record_count_) return nullptr;
        size_t low = 0;
        size_t high = blocks_.size();
        while (low + 1 < high) {
            const size_t mid = low + (high - low) / 2;
            if (blocks_[mid].first_record <= index) low = mid;
            else high = mid;
        }
        const auto &block = blocks_[low];
        return block.owner->record(index - block.first_record);
    }
    return index < record_count_ ? &records_[index] : nullptr;
}

const NightCatalogRecord *NightCatalog::find(SleepDayId sleep_day) const {
    if (!blocks_.empty()) {
        const auto *owner = record_owner(sleep_day);
        return owner ? owner->find(sleep_day) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = record_owner(record.sleep_day);
        count = 0;
        return owner ? owner->sessions(record, count) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = record_owner(record.sleep_day);
        count = 0;
        return owner ? owner->mask_windows(record, count) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = record_owner(record.sleep_day);
        count = 0;
        return owner ? owner->files(record, count) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = file_owner(file);
        count = 0;
        return owner ? owner->coverage(file, count) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = file_owner(file);
        count = 0;
        return owner ? owner->signal_layouts(file, count) : nullptr;
    }
    count = file.signal_layout_count;
    if (file.signal_layout_offset > signal_layout_count_ ||
        count > signal_layout_count_ - file.signal_layout_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? signal_layouts_ + file.signal_layout_offset : nullptr;
}

const char *NightCatalog::path(const NightCatalogSourceFile &file) const {
    if (!blocks_.empty()) {
        const auto *owner = file_owner(file);
        return owner ? owner->path(file) : nullptr;
    }
    if (file.path_length == 0 || file.path_offset >= path_bytes_ ||
        file.path_length >= path_bytes_ - file.path_offset) {
        return nullptr;
    }
    return paths_ + file.path_offset;
}

const NightCatalogFallbackFile *NightCatalog::fallback_files(
    const NightCatalogRecord &record,
    size_t &count) const {
    if (!blocks_.empty()) {
        const auto *owner = record_owner(record.sleep_day);
        count = 0;
        return owner ? owner->fallback_files(record, count) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = file_owner(file);
        count = 0;
        return owner ? owner->fallback_sections(file, count) : nullptr;
    }
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
    if (!blocks_.empty()) {
        const auto *owner = file_owner(file);
        return owner ? owner->path(file) : nullptr;
    }
    if (file.path_length == 0 || file.path_offset >= path_bytes_ ||
        file.path_length >= path_bytes_ - file.path_offset) {
        return nullptr;
    }
    return paths_ + file.path_offset;
}

uint32_t night_catalog_duration_minutes(
    const NightCatalog &catalog,
    const NightCatalogRecord &record) {
    if (record.metrics.has(NightCatalogMetric::DurationMinutes)) {
        return record.metrics.duration_min;
    }

    uint64_t duration_ms = 0;
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(record, session_count);
    for (size_t i = 0; sessions && i < session_count; ++i) {
        if (!sessions[i].valid()) continue;
        duration_ms += static_cast<uint64_t>(
            sessions[i].end_ms - sessions[i].start_ms);
    }
    return static_cast<uint32_t>(duration_ms / 60000);
}

}  // namespace aircannect
