#include "report_read_plan.h"

#include <limits>
#include <new>
#include <stdlib.h>
#include <type_traits>

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

size_t align_up(size_t value, size_t alignment) {
    if (alignment <= 1) return value;

    const size_t mask = alignment - 1;
    if (value > std::numeric_limits<size_t>::max() - mask) return 0;
    return (value + mask) & ~mask;
}

bool reserve_array(size_t &total,
                   size_t count,
                   size_t item_size,
                   size_t alignment,
                   size_t &offset) {
    offset = align_up(total, alignment);
    if (offset == 0 && total != 0) return false;
    if (count > std::numeric_limits<size_t>::max() / item_size) return false;

    const size_t bytes = count * item_size;
    if (offset > std::numeric_limits<size_t>::max() - bytes) return false;
    total = offset + bytes;
    return true;
}

void *allocate_plan_storage(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes, false);
#else
    return calloc(1, bytes);
#endif
}

void free_plan_storage(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

}  // namespace

ReportReadPlan::~ReportReadPlan() {
    free_plan_storage(storage_);
}

bool ReportReadPlan::allocate(size_t session_count,
                              size_t operation_count,
                              size_t mapping_count) {
    size_t total = 0;
    size_t sessions_offset = 0;
    size_t operations_offset = 0;
    size_t mappings_offset = 0;

    if (!reserve_array(total,
                       session_count,
                       sizeof(ReportReadSession),
                       alignof(ReportReadSession),
                       sessions_offset) ||
        !reserve_array(total,
                       operation_count,
                       sizeof(ReportReadOperation),
                       alignof(ReportReadOperation),
                       operations_offset) ||
        !reserve_array(total,
                       mapping_count,
                       sizeof(ReportReadMapping),
                       alignof(ReportReadMapping),
                       mappings_offset)) {
        return false;
    }

    if (total > 0) {
        storage_ = static_cast<uint8_t *>(allocate_plan_storage(total));
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
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog_->files(*night_, file_count);
    return files && operation.catalog_file_index < file_count
        ? &files[operation.catalog_file_index]
        : nullptr;
}

const char *ReportReadPlan::source_path(
    const ReportReadOperation &operation) const {
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
