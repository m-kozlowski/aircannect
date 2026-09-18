#pragma once

#include <memory>
#include <stdint.h>

#include "large_text_buffer.h"
#include "night_catalog.h"
#include "report_signal_store_catalog.h"

namespace aircannect {

// Prepared by the report worker; readers retain the same immutable bytes.
struct ReportCatalogJson {
    LargeTextBuffer body;
    char etag[64] = {};
    uint32_t generation = 0;
};

std::shared_ptr<const ReportCatalogJson> build_report_catalog_json(
    const NightCatalog &catalog,
    const ReportSignalStoreCatalog *store,
    uint32_t generation,
    std::shared_ptr<const ReportCatalogJson> previous = {});

}  // namespace aircannect
