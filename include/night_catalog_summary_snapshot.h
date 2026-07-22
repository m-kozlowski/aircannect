#pragma once

#include <memory>
#include <stddef.h>

#include "night_catalog_builder.h"
#include "report_proto.h"
#include "report_spool_types.h"

namespace aircannect {

// Compact immutable Summary input retained by the report task. The backing
// arrays live in one PSRAM-capable allocation on esp32-s3 targets.
class NightCatalogSummarySnapshot {
public:
    ~NightCatalogSummarySnapshot();

    NightCatalogSummarySnapshot(const NightCatalogSummarySnapshot &) = delete;
    NightCatalogSummarySnapshot &operator=(
        const NightCatalogSummarySnapshot &) = delete;

    size_t size() const { return record_count_; }
    const NightCatalogSummaryInput *records() const { return records_; }

    static std::shared_ptr<const NightCatalogSummarySnapshot> build(
        const ReportSummaryRecord *records,
        size_t record_count,
        char *error = nullptr,
        size_t error_size = 0);
    static std::shared_ptr<const NightCatalogSummarySnapshot> copy(
        const NightCatalogSummaryInput *records,
        size_t record_count);
    static std::shared_ptr<const NightCatalogSummarySnapshot> parse(
        const ReportSpoolResult &result,
        char *error = nullptr,
        size_t error_size = 0);
    static std::shared_ptr<const NightCatalogSummarySnapshot> from_catalog(
        const NightCatalog &catalog);

private:
    NightCatalogSummarySnapshot() = default;

    bool allocate(size_t record_count, size_t session_count);

    uint8_t *storage_ = nullptr;
    size_t storage_bytes_ = 0;
    NightCatalogSummaryInput *records_ = nullptr;
    NightCatalogTimeRange *sessions_ = nullptr;
    size_t record_count_ = 0;
    size_t session_count_ = 0;
};

}  // namespace aircannect
