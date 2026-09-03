#include "report_read_plan.h"

#include <new>
#include <type_traits>

#include "checked_size.h"
#include "memory_manager.h"

namespace aircannect {

ReportReadPlan::~ReportReadPlan() {
    Memory::free(storage_);
}

bool ReportReadPlan::allocate(size_t session_count,
                              size_t operation_count,
                              size_t mapping_count) {
    size_t total = 0;
    size_t sessions_offset = 0;
    size_t operations_offset = 0;
    size_t mappings_offset = 0;

    if (!CheckedSize::reserve_array<ReportReadSession>(
            total, session_count, sessions_offset) ||
        !CheckedSize::reserve_array<ReportReadOperation>(
            total, operation_count, operations_offset) ||
        !CheckedSize::reserve_array<ReportReadMapping>(
            total, mapping_count, mappings_offset)) {
        return false;
    }

    if (total > 0) {
        storage_ = static_cast<uint8_t *>(
            Memory::calloc_large(1, total, false));
        if (!storage_) return false;
    }

    storage_bytes_ = total;
    sessions_ = session_count > 0
        ? reinterpret_cast<ReportReadSession *>(storage_ + sessions_offset)
        : nullptr;
    operations_ = operation_count > 0
        ? reinterpret_cast<ReportReadOperation *>(storage_ + operations_offset)
        : nullptr;
    mappings_ = mapping_count > 0
        ? reinterpret_cast<ReportReadMapping *>(storage_ + mappings_offset)
        : nullptr;
    session_count_ = session_count;
    operation_count_ = operation_count;
    mapping_count_ = mapping_count;

    static_assert(std::is_trivially_destructible<ReportReadSession>::value,
                  "report read sessions must remain trivial");
    static_assert(std::is_trivially_destructible<ReportReadOperation>::value,
                  "report read operations must remain trivial");
    static_assert(std::is_trivially_destructible<ReportReadMapping>::value,
                  "report read mappings must remain trivial");

    for (size_t i = 0; i < session_count_; ++i) {
        new (&sessions_[i]) ReportReadSession();
    }
    for (size_t i = 0; i < operation_count_; ++i) {
        new (&operations_[i]) ReportReadOperation();
    }
    for (size_t i = 0; i < mapping_count_; ++i) {
        new (&mappings_[i]) ReportReadMapping();
    }
    return true;
}

const ReportReadSession *ReportReadPlan::session(size_t index) const {
    return index < session_count_ ? &sessions_[index] : nullptr;
}

const ReportReadOperation *ReportReadPlan::operation(size_t index) const {
    return index < operation_count_ ? &operations_[index] : nullptr;
}

const NightCatalogSourceFile *ReportReadPlan::source_file(
    const ReportReadOperation &operation) const {
    if (operation.kind == ReportReadOperationKind::FallbackSeries ||
        operation.kind == ReportReadOperationKind::FallbackSeriesSlice ||
        operation.kind == ReportReadOperationKind::FallbackEvents) {
        return nullptr;
    }

    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog_->files(*night_, file_count);
    return files && operation.catalog_file_index < file_count
        ? &files[operation.catalog_file_index]
        : nullptr;
}

const NightCatalogFallbackFile *ReportReadPlan::fallback_file(
    const ReportReadOperation &operation) const {
    if (operation.kind != ReportReadOperationKind::FallbackSeries &&
        operation.kind != ReportReadOperationKind::FallbackSeriesSlice &&
        operation.kind != ReportReadOperationKind::FallbackEvents) {
        return nullptr;
    }

    size_t file_count = 0;
    const NightCatalogFallbackFile *files =
        catalog_->fallback_files(*night_, file_count);
    return files && operation.catalog_file_index < file_count
        ? &files[operation.catalog_file_index]
        : nullptr;
}

const NightCatalogFallbackSection *ReportReadPlan::fallback_section(
    const ReportReadOperation &operation) const {
    const NightCatalogFallbackFile *file = fallback_file(operation);
    if (!file) return nullptr;

    size_t section_count = 0;
    const NightCatalogFallbackSection *sections =
        catalog_->fallback_sections(*file, section_count);
    return sections && operation.fallback_section_index < section_count
        ? &sections[operation.fallback_section_index]
        : nullptr;
}

const char *ReportReadPlan::source_path(
    const ReportReadOperation &operation) const {
    const NightCatalogFallbackFile *fallback = fallback_file(operation);
    if (fallback) return catalog_->path(*fallback);

    const NightCatalogSourceFile *file = source_file(operation);
    return file ? catalog_->path(*file) : nullptr;
}

const ReportReadMapping *ReportReadPlan::mapping(size_t index) const {
    return index < mapping_count_ ? &mappings_[index] : nullptr;
}

const ReportReadMapping *ReportReadPlan::mappings(
    const ReportReadOperation &operation,
    size_t &count) const {
    count = operation.mapping_count;
    if (operation.mapping_offset > mapping_count_ ||
        count > mapping_count_ - operation.mapping_offset) {
        count = 0;
        return nullptr;
    }
    return count > 0 ? mappings_ + operation.mapping_offset : nullptr;
}

}  // namespace aircannect
