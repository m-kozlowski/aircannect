#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "report_signal_store_catalog.h"

namespace aircannect {

struct ReportCatalogJson {
    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const ReportSignalStoreCatalog> store;
    char etag[64] = {};
    uint32_t generation = 0;
    size_t content_length = 0;

    size_t length() const { return content_length; }
};

class ReportCatalogJsonReader {
public:
    explicit ReportCatalogJsonReader(
        std::shared_ptr<const ReportCatalogJson> publication);

    size_t read(uint8_t *buffer, size_t capacity, size_t offset);
    bool valid() const { return !failed_; }
    bool complete() const;

private:
    enum class Phase : uint8_t {
        Prefix,
        Nights,
        NightPrefix,
        Sessions,
        Means,
        NightEnd,
        Done,
    };

    static constexpr size_t ScratchBytes = 256;

    bool prepare_fragment();
    bool format_fragment(const char *format, ...);
    bool append_format(const char *format, ...);
    bool append_text(const char *text);

    std::shared_ptr<const ReportCatalogJson> publication_;
    size_t offset_ = 0;
    size_t night_index_ = 0;
    size_t session_index_ = 0;
    const NightCatalogRecord *current_night_ = nullptr;
    const ReportSignalStoreCatalogRecord *current_stored_ = nullptr;
    size_t fragment_length_ = 0;
    size_t fragment_offset_ = 0;
    Phase phase_ = Phase::Prefix;
    bool first_night_ = true;
    bool failed_ = false;
    char fragment_[ScratchBytes] = {};
};

std::shared_ptr<const ReportCatalogJson> build_report_catalog_json(
    std::shared_ptr<const NightCatalog> catalog,
    std::shared_ptr<const ReportSignalStoreCatalog> store,
    uint32_t generation,
    std::shared_ptr<const ReportCatalogJson> previous = {});

}  // namespace aircannect
