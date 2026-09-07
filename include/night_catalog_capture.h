#pragma once

#include <stdint.h>

#include <memory>

#include "edf_session_metadata.h"
#include "edf_storage_progress.h"
#include "night_catalog.h"

namespace aircannect {

class NightCatalogCapture {
public:
    // Return the UTC-aligned quarter boundary safe for publication, or zero
    // when the current session has no usable primary numeric coverage.
    static int64_t closed_end(const EdfSessionMetadata &metadata,
                              const EdfStorageProgress &progress);

    static std::shared_ptr<const NightCatalog> build(
        const std::shared_ptr<const NightCatalog> &previous,
        const EdfSessionMetadata &metadata,
        const EdfStorageProgress &progress,
        int64_t publication_end_ms);
};

}  // namespace aircannect
