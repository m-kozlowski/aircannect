#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog.h"
#include "report_artifact_key.h"
#include "report_sources.h"

namespace aircannect {

enum class ReportReadQuality : uint8_t {
    Primary,
    Fallback,
};

enum class ReportReadOperationKind : uint8_t {
    Numeric,
    ScoredEvents,
    CsrEvents,
    FallbackSeries,
    FallbackEvents,
};

struct ReportReadSession {
    NightCatalogTimeRange output_window;
    uint16_t catalog_session_index = 0;
    uint32_t selected_signal_mask = 0;
    uint32_t complete_signal_mask = 0;
    uint32_t fallback_signal_mask = 0;
    uint32_t missing_signal_mask = 0;
    uint32_t unavailable_signal_mask = 0;
    uint8_t captured_event_mask = 0;
    uint8_t missing_event_mask = 0;
};

struct ReportReadMapping {
    NightCatalogTimeRange output_window;
    ReportSeriesDescriptor series;
    EdfReportSignalLayout layout;
};

struct ReportReadOperation {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint32_t first_record = 0;
    uint32_t record_count = 0;
    uint32_t mapping_offset = 0;
    uint16_t mapping_count = 0;
    uint16_t session_index = 0;
    uint16_t catalog_file_index = 0;
    uint16_t fallback_section_index = 0;
    ReportReadOperationKind kind = ReportReadOperationKind::Numeric;
    uint8_t event_mask = 0;
    NightCatalogTimeRange event_filter;
};

class ReportPlanner;

class ReportReadPlan {
public:
    ~ReportReadPlan();

    ReportReadPlan(const ReportReadPlan &) = delete;
    ReportReadPlan &operator=(const ReportReadPlan &) = delete;

    const ReportArtifactKey &key() const { return key_; }
    size_t storage_bytes() const { return storage_bytes_; }

    uint32_t requested_signal_mask() const { return requested_signal_mask_; }
    uint32_t missing_required_signal_mask() const {
        return missing_required_signal_mask_;
    }
    uint32_t missing_optional_signal_mask() const {
        return missing_optional_signal_mask_;
    }
    uint32_t unavailable_signal_mask() const {
        return unavailable_signal_mask_;
    }
    uint32_t acquirable_signal_mask() const {
        return acquirable_signal_mask_;
    }
    bool fallback_acquisition_allowed() const {
        return fallback_acquisition_allowed_;
    }
    uint8_t requested_event_mask() const { return requested_event_mask_; }
    uint8_t missing_event_mask() const { return missing_event_mask_; }

    size_t session_count() const { return session_count_; }
    const ReportReadSession *session(size_t index) const;

    size_t operation_count() const { return operation_count_; }
    const ReportReadOperation *operation(size_t index) const;
    const NightCatalogSourceFile *source_file(
        const ReportReadOperation &operation) const;
    const NightCatalogFallbackFile *fallback_file(
        const ReportReadOperation &operation) const;
    const NightCatalogFallbackSection *fallback_section(
        const ReportReadOperation &operation) const;
    const char *source_path(const ReportReadOperation &operation) const;

    size_t mapping_count() const { return mapping_count_; }
    const ReportReadMapping *mapping(size_t index) const;
    const ReportReadMapping *mappings(const ReportReadOperation &operation,
                                      size_t &count) const;

    const NightCatalog &catalog() const { return *catalog_; }
    const NightCatalogRecord &night() const { return *night_; }

private:
    ReportReadPlan() = default;

    bool allocate(size_t session_count,
                  size_t operation_count,
                  size_t mapping_count);

    std::shared_ptr<const NightCatalog> catalog_;
    const NightCatalogRecord *night_ = nullptr;
    ReportArtifactKey key_;

    uint8_t *storage_ = nullptr;
    size_t storage_bytes_ = 0;
    ReportReadSession *sessions_ = nullptr;
    ReportReadOperation *operations_ = nullptr;
    ReportReadMapping *mappings_ = nullptr;
    size_t session_count_ = 0;
    size_t operation_count_ = 0;
    size_t mapping_count_ = 0;

    uint32_t requested_signal_mask_ = 0;
    uint32_t missing_required_signal_mask_ = 0;
    uint32_t missing_optional_signal_mask_ = 0;
    uint32_t unavailable_signal_mask_ = 0;
    uint32_t acquirable_signal_mask_ = 0;
    bool fallback_acquisition_allowed_ = false;
    uint8_t requested_event_mask_ = 0;
    uint8_t missing_event_mask_ = 0;

    friend class ReportPlanner;
};

}  // namespace aircannect
