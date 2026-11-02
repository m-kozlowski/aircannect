#include "report_signal_store_catalog.h"

#include <algorithm>
#include <new>
#include <utility>

#include "checked_size.h"
#include "large_allocator.h"
#include "memory_manager.h"

namespace aircannect {
ReportSignalStoreCatalogRecord ReportSignalStoreCatalogBuilder::project(
    const ReportSignalStoreMetadata &input) {
    ReportSignalStoreCatalogRecord record;
    const auto &night = input.view.night;
    record.sleep_day = night.sleep_day;
    record.source_revision = night.source_revision;
    record.generation = night.generation;
    record.rejected_source_revision = night.rejected_source_revision;
    record.flags = night.flags;
    record.metadata_size = input.metadata ? input.metadata->size() : 0;
    record.compression_version = night.compression_version;
    record.source_flags = night.source_flags;
    record.checkpoint_slot = night.checkpoint_slot;

    const auto &m = night.metrics;
    const auto &s = night.statistics;
    const int32_t values[] = {m.ahi_milli, m.ipap_mean_milli,
        m.leak_mean_milli, s.tidal_volume.mean_milli, s.minute_ventilation.mean_milli};
    const bool valid[] = {(m.valid_mask & 1u) != 0,
        (m.valid_mask & REPORT_NIGHT_METRIC_IPAP_MEAN) != 0,
        (m.valid_mask & REPORT_NIGHT_METRIC_LEAK_MEAN) != 0,
        s.tidal_volume.valid, s.minute_ventilation.valid};
    for (size_t i = 0; i < 5; ++i) {
        record.trend_values[i] = values[i];
        if (valid[i]) record.trend_valid_mask |= 1u << i;
    }
    return record;
}

ReportSignalStoreCatalog::~ReportSignalStoreCatalog() {
    for (size_t i = 0; i < record_count_; ++i) {
        records_[i].~ReportSignalStoreCatalogRecord();
    }
    Memory::free(records_);
}

bool ReportSignalStoreCatalog::allocate(size_t record_count) {
    size_t bytes = 0;
    if (!CheckedSize::multiply(record_count,
                               sizeof(ReportSignalStoreCatalogRecord),
                               bytes)) {
        return false;
    }

    if (bytes > 0) {
        records_ = static_cast<ReportSignalStoreCatalogRecord *>(
            Memory::alloc_large(bytes, false));
        if (!records_) return false;

        for (size_t i = 0; i < record_count; ++i) {
            new (records_ + i) ReportSignalStoreCatalogRecord();
        }
    }

    record_count_ = record_count;
    storage_bytes_ = bytes;
    return true;
}

const ReportSignalStoreCatalogRecord *ReportSignalStoreCatalog::find(
    SleepDayId sleep_day) const {
    if (!sleep_day.valid()) return nullptr;

    size_t left = 0;
    size_t right = record_count_;
    while (left < right) {
        const size_t middle = left + (right - left) / 2;
        const SleepDayId candidate = records_[middle].sleep_day;
        if (candidate == sleep_day) return records_ + middle;
        if (candidate < sleep_day) {
            right = middle;
        } else {
            left = middle + 1;
        }
    }
    return nullptr;
}

bool ReportSignalStoreCatalog::ready(
    SleepDayId sleep_day,
    SourceRevision source_revision) const {
    const ReportSignalStoreCatalogRecord *record = find(sleep_day);
    return record && record->source_revision == source_revision &&
        !(record->flags & REPORT_SIGNAL_STORE_NIGHT_TIME_UNRESOLVED);
}

const ReportSignalStoreMetadata *ReportSignalStoreCatalog::cached(SleepDayId day) const {
    for (const auto &entry : cache_) {
        if (entry && entry->view.night.sleep_day == day) return entry.get();
    }
    return nullptr;
}

size_t ReportSignalStoreCatalog::metadata_bytes() const {
    size_t bytes = 0;
    for (const auto &entry : cache_) {
        if (entry) bytes += entry->metadata->size() + sizeof(*entry);
    }
    return bytes;
}

void ReportSignalStoreCatalog::copy_cache(const ReportSignalStoreCatalog &source) {
    size_t count = 0;
    for (const auto &entry : source.cache_) {
        if (entry && find(entry->view.night.sleep_day)) cache_[count++] = entry;
    }
}

void ReportSignalStoreCatalog::remember(const ReportSignalStoreMetadata &metadata) {
    if (!metadata.metadata) return;

    const auto day = metadata.view.night.sleep_day;
    for (auto &entry : cache_) {
        if (entry && entry->view.night.sleep_day == day) entry.reset();
    }
    // The newest accepted night always fits; older cache entries are optional.
    auto next = std::allocate_shared<ReportSignalStoreMetadata>(
        LargeAllocator<ReportSignalStoreMetadata>(), metadata);
    size_t bytes = metadata.metadata->size();
    size_t output = 1;
    for (size_t i = 0; i < CacheEntries; ++i) {
        auto entry = std::move(cache_[i]);
        if (!entry || output >= CacheEntries ||
            bytes + entry->metadata->size() > CacheBytes) continue;
        bytes += entry->metadata->size();
        // Compact towards the front, then shift once to insert the new entry.
        cache_[output - 1] = std::move(entry);
        ++output;
    }
    for (size_t i = output - 1; i > 0; --i) cache_[i] = std::move(cache_[i - 1]);
    cache_[0] = std::move(next);
    for (size_t i = output; i < CacheEntries; ++i) cache_[i].reset();
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::build(
    const ReportSignalStoreCatalogInput *inputs,
    size_t input_count) try {
    if (input_count > 0 && !inputs) return {};

    auto catalog = std::allocate_shared<ReportSignalStoreCatalog>(
        LargeAllocator<ReportSignalStoreCatalog>());
    if (!catalog || !catalog->allocate(input_count)) return {};

    for (size_t i = 0; i < input_count; ++i) {
        if (!inputs[i].metadata) return {};
        catalog->records_[i] = project(inputs[i]);
        catalog->remember(inputs[i]);
    }

    if (input_count > 1) {
        std::sort(catalog->records_, catalog->records_ + input_count,
                  [](const ReportSignalStoreCatalogRecord &left,
                     const ReportSignalStoreCatalogRecord &right) {
                      return right.sleep_day < left.sleep_day;
                  });
    }

    for (size_t i = 1; i < input_count; ++i) {
        if (catalog->records_[i - 1].sleep_day ==
            catalog->records_[i].sleep_day) {
            return {};
        }
    }
    return catalog;
} catch (const std::bad_alloc &) {
    return {};
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::build_records(
    const ReportSignalStoreCatalogRecord *records, size_t count,
    const ReportSignalStoreCatalog *known) try {
    auto catalog = std::allocate_shared<ReportSignalStoreCatalog>(
        LargeAllocator<ReportSignalStoreCatalog>());
    if (!catalog || !catalog->allocate(count)) return {};
    if (count) std::copy_n(records, count, catalog->records_);
    if (known) catalog->copy_cache(*known);
    return catalog;
} catch (const std::bad_alloc &) {
    return {};
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::upsert(
    const ReportSignalStoreCatalog &source,
    const ReportSignalStoreCatalogInput &input) try {
    if (!input.metadata) return {};
    const auto input_record = project(input);

    const ReportSignalStoreCatalogRecord *replaced =
        source.find(input_record.sleep_day);
    const size_t count = source.record_count_ + (replaced ? 0 : 1);
    auto catalog = std::allocate_shared<ReportSignalStoreCatalog>(
        LargeAllocator<ReportSignalStoreCatalog>());
    if (!catalog || !catalog->allocate(count)) return {};

    size_t source_index = 0;
    size_t output = 0;
    bool inserted = false;
    while (output < count) {
        while (source_index < source.record_count_ && replaced &&
               source.records_ + source_index == replaced) {
            ++source_index;
        }

        const bool use_input = !inserted &&
            (source_index >= source.record_count_ ||
             source.records_[source_index].sleep_day <
                 input_record.sleep_day);
        if (use_input) {
            catalog->records_[output] = std::move(input_record);
            inserted = true;
        } else {
            if (source_index >= source.record_count_) return {};
            catalog->records_[output] = source.records_[source_index++];
        }
        ++output;
    }
    catalog->copy_cache(source);
    catalog->remember(input);
    return inserted ? catalog : nullptr;
} catch (const std::bad_alloc &) {
    return {};
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::reconcile(
    std::shared_ptr<const ReportSignalStoreCatalog> source,
    const NightCatalog &night_catalog) try {
    if (!source) return {};

    // Stale metadata retains generation history, but ready() checks revision.
    size_t count = 0;
    for (size_t i = 0; i < source->record_count_; ++i) {
        const ReportSignalStoreCatalogRecord &record = source->records_[i];
        const NightCatalogRecord *night = night_catalog.find(record.sleep_day);
        if (night) ++count;
    }
    if (count == source->record_count_) return source;

    auto catalog = std::allocate_shared<ReportSignalStoreCatalog>(
        LargeAllocator<ReportSignalStoreCatalog>());
    if (!catalog || !catalog->allocate(count)) return {};

    size_t output = 0;
    for (size_t i = 0; i < source->record_count_; ++i) {
        const ReportSignalStoreCatalogRecord &record = source->records_[i];
        const NightCatalogRecord *night = night_catalog.find(record.sleep_day);
        if (!night) continue;

        catalog->records_[output++] = record;
    }
    catalog->copy_cache(*source);
    return output == count ? catalog : nullptr;
} catch (const std::bad_alloc &) {
    return {};
}

}  // namespace aircannect
