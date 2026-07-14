#include "report_night_index_snapshot.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>
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

bool add_bytes(size_t &total, size_t count, size_t item_size) {
    if (count > std::numeric_limits<size_t>::max() / item_size) return false;

    const size_t bytes = count * item_size;
    if (total > std::numeric_limits<size_t>::max() - bytes) return false;
    total += bytes;
    return true;
}

void *allocate_snapshot_storage(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes, false);
#else
    return calloc(1, bytes);
#endif
}

void free_snapshot_storage(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

struct ArrayReaderContext {
    const ReportIndexedNight *nights = nullptr;
};

bool read_array_night(void *context,
                      size_t index,
                      ReportIndexedNight &out) {
    const auto *array = static_cast<const ArrayReaderContext *>(context);
    if (!array || !array->nights) return false;

    out = array->nights[index];
    return true;
}

class ScopedNightScratch {
public:
    ScopedNightScratch()
        : night_(static_cast<ReportIndexedNight *>(
              allocate_snapshot_storage(sizeof(ReportIndexedNight)))) {
        if (night_) new (night_) ReportIndexedNight();
    }

    ~ScopedNightScratch() {
        if (night_) night_->~ReportIndexedNight();
        free_snapshot_storage(night_);
    }

    ScopedNightScratch(const ScopedNightScratch &) = delete;
    ScopedNightScratch &operator=(const ScopedNightScratch &) = delete;

    explicit operator bool() const { return night_ != nullptr; }
    ReportIndexedNight &get() { return *night_; }

private:
    ReportIndexedNight *night_ = nullptr;
};

}  // namespace

ReportNightIndexSnapshot::~ReportNightIndexSnapshot() {
    for (size_t i = 0; entries_ && i < count_; ++i) {
        entries_[i].~Entry();
    }
    free_snapshot_storage(storage_);
}

std::shared_ptr<const ReportNightIndexSnapshot>
ReportNightIndexSnapshot::create(const ReportIndexedNight *nights,
                                 size_t count) {
    if (!nights && count > 0) return {};

    ArrayReaderContext context{nights};
    return create_from_reader(count, read_array_night, &context);
}

std::shared_ptr<const ReportNightIndexSnapshot>
ReportNightIndexSnapshot::create_from_reader(size_t count,
                                             ReadNightFn reader,
                                             void *context) {
    if (!reader && count > 0) return {};

    std::shared_ptr<ReportNightIndexSnapshot> snapshot(
        new (std::nothrow) ReportNightIndexSnapshot());
    if (!snapshot || !snapshot->initialize(count, reader, context)) return {};

    return snapshot;
}

bool ReportNightIndexSnapshot::initialize(size_t count,
                                          ReadNightFn reader,
                                          void *context) {
    ScopedNightScratch scratch;
    if (count > 0 && !scratch) return false;

    size_t total_ranges = 0;
    size_t total_signatures = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!reader(context, i, scratch.get())) return false;

        const ReportIndexedNight &night = scratch.get();
        const size_t range_count = std::min(
            night.range_count,
            static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
        const size_t data_range_count = std::min(
            night.data_range_count,
            static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
        const size_t signature_count = std::min(
            night.edf_source_signature_count,
            static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));

        if (total_ranges > std::numeric_limits<size_t>::max() - range_count ||
            total_ranges + range_count >
                std::numeric_limits<size_t>::max() - data_range_count ||
            total_signatures >
                std::numeric_limits<size_t>::max() - signature_count) {
            return false;
        }

        total_ranges += range_count + data_range_count;
        total_signatures += signature_count;
    }
    if (total_ranges > std::numeric_limits<uint32_t>::max() ||
        total_signatures > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    size_t entries_offset = 0;
    size_t total_bytes = 0;
    if (!add_bytes(total_bytes, count, sizeof(Entry))) return false;

    const size_t ranges_offset =
        align_up(total_bytes, alignof(ReportSessionRange));
    if (ranges_offset == 0 && total_bytes != 0) return false;
    total_bytes = ranges_offset;
    if (!add_bytes(total_bytes,
                   total_ranges,
                   sizeof(ReportSessionRange))) {
        return false;
    }

    const size_t signatures_offset = align_up(total_bytes, alignof(uint64_t));
    if (signatures_offset == 0 && total_bytes != 0) return false;
    total_bytes = signatures_offset;
    if (!add_bytes(total_bytes, total_signatures, sizeof(uint64_t))) return false;

    if (total_bytes > 0) {
        storage_ = static_cast<uint8_t *>(
            allocate_snapshot_storage(total_bytes));
        if (!storage_) return false;
    }

    storage_bytes_ = total_bytes;
    entries_ = count > 0
        ? reinterpret_cast<Entry *>(storage_ + entries_offset)
        : nullptr;
    ranges_ = total_ranges > 0
        ? reinterpret_cast<ReportSessionRange *>(storage_ + ranges_offset)
        : nullptr;
    signatures_ = total_signatures > 0
        ? reinterpret_cast<uint64_t *>(storage_ + signatures_offset)
        : nullptr;
    count_ = count;
    range_count_ = total_ranges;
    signature_count_ = total_signatures;

    static_assert(std::is_trivially_destructible<Entry>::value,
                  "night-index snapshot entries must remain trivial");
    for (size_t i = 0; i < count_; ++i) {
        new (&entries_[i]) Entry();
    }
    for (size_t i = 0; i < range_count_; ++i) {
        new (&ranges_[i]) ReportSessionRange();
    }
    for (size_t i = 0; i < signature_count_; ++i) {
        new (&signatures_[i]) uint64_t(0);
    }

    size_t next_range = 0;
    size_t next_signature = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!reader(context, i, scratch.get())) return false;

        const ReportIndexedNight &night = scratch.get();
        Entry &entry = entries_[i];

        entry.summary = night.summary;
        entry.range_offset = static_cast<uint32_t>(next_range);
        entry.range_count = static_cast<uint32_t>(std::min(
            night.range_count,
            static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX)));
        if (entry.range_count > 0) {
            memcpy(ranges_ + next_range,
                   night.ranges,
                   entry.range_count * sizeof(ReportSessionRange));
            next_range += entry.range_count;
        }

        entry.data_range_offset = static_cast<uint32_t>(next_range);
        entry.data_range_count = static_cast<uint32_t>(std::min(
            night.data_range_count,
            static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX)));
        if (entry.data_range_count > 0) {
            memcpy(ranges_ + next_range,
                   night.data_ranges,
                   entry.data_range_count * sizeof(ReportSessionRange));
            next_range += entry.data_range_count;
        }

        entry.signature_offset = static_cast<uint32_t>(next_signature);
        entry.signature_count = static_cast<uint32_t>(std::min(
            night.edf_source_signature_count,
            static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX)));
        if (entry.signature_count > 0) {
            memcpy(signatures_ + next_signature,
                   night.edf_source_signatures,
                   entry.signature_count * sizeof(uint64_t));
            next_signature += entry.signature_count;
        }

        entry.source_signature = night.source_signature;
        entry.has_summary = night.has_summary;
        entry.has_edf = night.has_edf;
        entry.edf_catalog_pending = night.edf_catalog_pending;
        if (entry.summary.valid &&
            entry.summary.duration_min > 0 &&
            entry.range_count > 0) {
            therapy_night_count_++;
        }
    }

    return next_range == range_count_ &&
           next_signature == signature_count_;
}

const ReportSummaryRecord *ReportNightIndexSnapshot::summary_at(
    size_t index) const {
    if (index >= count_ || !entries_) return nullptr;
    return &entries_[index].summary;
}

bool ReportNightIndexSnapshot::materialize(size_t index,
                                           ReportIndexedNight &out) const {
    if (index >= count_ || !entries_) return false;

    const Entry &entry = entries_[index];
    out = {};
    out.summary = entry.summary;
    out.range_count = entry.range_count;
    if (entry.range_count > 0) {
        memcpy(out.ranges,
               ranges_ + entry.range_offset,
               entry.range_count * sizeof(ReportSessionRange));
    }

    out.data_range_count = entry.data_range_count;
    if (entry.data_range_count > 0) {
        memcpy(out.data_ranges,
               ranges_ + entry.data_range_offset,
               entry.data_range_count * sizeof(ReportSessionRange));
    }

    out.edf_source_signature_count = entry.signature_count;
    if (entry.signature_count > 0) {
        memcpy(out.edf_source_signatures,
               signatures_ + entry.signature_offset,
               entry.signature_count * sizeof(uint64_t));
    }

    out.source_signature = entry.source_signature;
    out.has_summary = entry.has_summary;
    out.has_edf = entry.has_edf;
    out.edf_catalog_pending = entry.edf_catalog_pending;
    return true;
}

bool ReportNightIndexSnapshot::visible(size_t index) const {
    if (index >= count_ || !entries_) return false;

    const Entry &entry = entries_[index];
    return entry.summary.valid &&
           entry.summary.duration_min > 0 &&
           entry.range_count > 0;
}

bool ReportNightIndexSnapshot::by_therapy_index(
    size_t therapy_index,
    ReportIndexedNight &out) const {
    size_t current = 0;
    for (size_t i = count_; i > 0; --i) {
        const size_t index = i - 1;
        if (!visible(index)) continue;

        if (current == therapy_index) return materialize(index, out);
        current++;
    }

    return false;
}

bool ReportNightIndexSnapshot::by_start(
    uint64_t night_start_ms,
    ReportIndexedNight &out,
    size_t *therapy_index_out) const {
    size_t current = 0;
    for (size_t i = count_; i > 0; --i) {
        const size_t index = i - 1;
        if (!visible(index)) continue;

        if (entries_[index].summary.start_ms == night_start_ms) {
            if (!materialize(index, out)) return false;
            if (therapy_index_out) *therapy_index_out = current;
            return true;
        }
        current++;
    }

    return false;
}

}  // namespace aircannect
