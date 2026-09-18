#pragma once

#include <stdint.h>

#include <memory>

#include "edf_session_metadata.h"
#include "edf_storage_progress.h"
#include "night_catalog.h"

namespace aircannect {

class NightCatalogCapture {
public:
    NightCatalogCapture() = default;
    ~NightCatalogCapture();
    NightCatalogCapture(const NightCatalogCapture &) = delete;
    NightCatalogCapture &operator=(const NightCatalogCapture &) = delete;

    void reset();
    bool prepare(const EdfSessionMetadata &metadata,
                 std::shared_ptr<const EdfStorageProgress> progress);

    // Return the UTC-aligned quarter boundary safe for publication, or zero
    // when the current session has no usable primary numeric coverage.
    int64_t closed_end() const;
    bool publication_due(const std::shared_ptr<const NightCatalog> &previous,
                         int64_t published_end_ms,
                         const EdfStorageProgress *published_progress,
                         bool &rewritten) const;

    std::shared_ptr<const NightCatalog> build(
        const std::shared_ptr<const NightCatalog> &previous,
        int64_t publication_end_ms);

private:
    struct Runtime;
    Runtime *runtime_ = nullptr;
};

}  // namespace aircannect
