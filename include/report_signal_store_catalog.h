#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "report_signal_store.h"

namespace aircannect {

struct ReportSignalStoreCatalogRecord {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    SourceRevision rejected_source_revision;
    uint32_t flags = 0;
    uint32_t metadata_size = 0;
    uint16_t compression_version = 0;
    uint8_t source_flags = 0;
    uint8_t checkpoint_slot = 0;
    int32_t trend_values[5] = {};
    uint8_t trend_valid_mask = 0;
};

using ReportSignalStoreCatalogInput = ReportSignalStoreMetadata;

class ReportSignalStoreCatalogBuilder;

class ReportSignalStoreCatalog {
public:
    ReportSignalStoreCatalog() = default;
    ~ReportSignalStoreCatalog();

    ReportSignalStoreCatalog(const ReportSignalStoreCatalog &) = delete;
    ReportSignalStoreCatalog &operator=(
        const ReportSignalStoreCatalog &) = delete;

    size_t size() const { return record_count_; }
    size_t storage_bytes() const { return storage_bytes_; }
    const ReportSignalStoreCatalogRecord *record(size_t index) const {
        return index < record_count_ ? records_ + index : nullptr;
    }

    const ReportSignalStoreCatalogRecord *find(
        SleepDayId sleep_day) const;
    bool ready(SleepDayId sleep_day, SourceRevision source_revision) const;
    const ReportSignalStoreMetadata *cached(SleepDayId sleep_day) const;
    size_t metadata_bytes() const;

private:
    bool allocate(size_t record_count);
    void remember(const ReportSignalStoreMetadata &metadata);
    void copy_cache(const ReportSignalStoreCatalog &source);

    ReportSignalStoreCatalogRecord *records_ = nullptr;
    size_t record_count_ = 0;
    size_t storage_bytes_ = 0;
    static constexpr size_t CacheEntries = 4;
    static constexpr size_t CacheBytes = 64 * 1024;
    std::shared_ptr<const ReportSignalStoreMetadata> cache_[CacheEntries];

    friend class ReportSignalStoreCatalogBuilder;
};

class ReportSignalStoreCatalogBuilder {
public:
    static ReportSignalStoreCatalogRecord project(
        const ReportSignalStoreMetadata &metadata);
    // Discovery supplies records in the source catalog's descending day order.
    static std::shared_ptr<const ReportSignalStoreCatalog> build_records(
        const ReportSignalStoreCatalogRecord *records, size_t count,
        const ReportSignalStoreCatalog *known = nullptr);
    static std::shared_ptr<const ReportSignalStoreCatalog> build(
        const ReportSignalStoreCatalogInput *inputs,
        size_t input_count);
    static std::shared_ptr<const ReportSignalStoreCatalog> upsert(
        const ReportSignalStoreCatalog &source,
        const ReportSignalStoreCatalogInput &input);
    static std::shared_ptr<const ReportSignalStoreCatalog> reconcile(
        std::shared_ptr<const ReportSignalStoreCatalog> source,
        const NightCatalog &catalog);
};

}  // namespace aircannect
