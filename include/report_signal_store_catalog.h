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
    std::shared_ptr<const LargeByteBuffer> metadata;
};

struct ReportSignalStoreCatalogInput {
    std::shared_ptr<const LargeByteBuffer> metadata;
};

class ReportSignalStoreCatalogBuilder;

class ReportSignalStoreCatalog {
public:
    ~ReportSignalStoreCatalog();

    ReportSignalStoreCatalog(const ReportSignalStoreCatalog &) = delete;
    ReportSignalStoreCatalog &operator=(
        const ReportSignalStoreCatalog &) = delete;

    size_t size() const { return record_count_; }
    size_t storage_bytes() const { return storage_bytes_; }

    const ReportSignalStoreCatalogRecord *find(
        SleepDayId sleep_day) const;
    bool ready(SleepDayId sleep_day, SourceRevision source_revision) const;

private:
    ReportSignalStoreCatalog() = default;

    bool allocate(size_t record_count);

    ReportSignalStoreCatalogRecord *records_ = nullptr;
    size_t record_count_ = 0;
    size_t storage_bytes_ = 0;

    friend class ReportSignalStoreCatalogBuilder;
};

class ReportSignalStoreCatalogBuilder {
public:
    static std::shared_ptr<const ReportSignalStoreCatalog> build(
        const ReportSignalStoreCatalogInput *inputs,
        size_t input_count);
    static std::shared_ptr<const ReportSignalStoreCatalog> upsert(
        const ReportSignalStoreCatalog &source,
        const ReportSignalStoreCatalogInput &input);
    static std::shared_ptr<const ReportSignalStoreCatalog> reconcile(
        const ReportSignalStoreCatalog &source,
        const NightCatalog &catalog);
};

}  // namespace aircannect
