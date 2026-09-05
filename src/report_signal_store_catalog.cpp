#include "report_signal_store_catalog.h"

#include <algorithm>
#include <new>
#include <utility>

#include "checked_size.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

bool fill_record(ReportSignalStoreCatalogRecord &record,
                 const ReportSignalStoreCatalogInput &input) {
    ReportSignalStoreNightView view;
    if (!input.metadata ||
        !ReportSignalStoreNightCodec::decode(
            input.metadata->data(), input.metadata->size(), view)) {
        return false;
    }

    record.sleep_day = view.night.sleep_day;
    record.source_revision = view.night.source_revision;
    record.generation = view.night.generation;
    record.metadata = input.metadata;
    return true;
}

}  // namespace

ReportSignalStoreCatalog::~ReportSignalStoreCatalog() {
    for (size_t i = 0; i < record_count_; ++i) {
        records_[i].~ReportSignalStoreCatalogRecord();
    }
    Memory::free(records_);
}

bool ReportSignalStoreCatalog::allocate(size_t record_count) {
    size_t bytes = 0;
    if (!CheckedSize::multiply(record_count,
                               sizeof(ReportSignalStoreCatalogRecord),
                               bytes)) {
        return false;
    }

    if (bytes > 0) {
        records_ = static_cast<ReportSignalStoreCatalogRecord *>(
            Memory::alloc_large(bytes, false));
        if (!records_) return false;

        for (size_t i = 0; i < record_count; ++i) {
            new (records_ + i) ReportSignalStoreCatalogRecord();
        }
    }

    record_count_ = record_count;
    storage_bytes_ = bytes;
    return true;
}

const ReportSignalStoreCatalogRecord *ReportSignalStoreCatalog::find(
    SleepDayId sleep_day) const {
    if (!sleep_day.valid()) return nullptr;

    size_t left = 0;
    size_t right = record_count_;
    while (left < right) {
        const size_t middle = left + (right - left) / 2;
        const SleepDayId candidate = records_[middle].sleep_day;
        if (candidate == sleep_day) return records_ + middle;
        if (candidate < sleep_day) {
            right = middle;
        } else {
            left = middle + 1;
        }
    }
    return nullptr;
}

bool ReportSignalStoreCatalog::ready(
    SleepDayId sleep_day,
    SourceRevision source_revision) const {
    const ReportSignalStoreCatalogRecord *record = find(sleep_day);
    return record && record->source_revision == source_revision;
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::build(
    const ReportSignalStoreCatalogInput *inputs,
    size_t input_count) {
    if (input_count > 0 && !inputs) return {};

    std::shared_ptr<ReportSignalStoreCatalog> catalog(
        new (std::nothrow) ReportSignalStoreCatalog());
    if (!catalog || !catalog->allocate(input_count)) return {};

    for (size_t i = 0; i < input_count; ++i) {
        if (!fill_record(catalog->records_[i], inputs[i])) return {};
    }

    if (input_count > 1) {
        std::sort(catalog->records_, catalog->records_ + input_count,
                  [](const ReportSignalStoreCatalogRecord &left,
                     const ReportSignalStoreCatalogRecord &right) {
                      return right.sleep_day < left.sleep_day;
                  });
    }

    for (size_t i = 1; i < input_count; ++i) {
        if (catalog->records_[i - 1].sleep_day ==
            catalog->records_[i].sleep_day) {
            return {};
        }
    }
    return catalog;
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::upsert(
    const ReportSignalStoreCatalog &source,
    const ReportSignalStoreCatalogInput &input) {
    ReportSignalStoreCatalogRecord input_record;
    if (!fill_record(input_record, input)) return {};

    const ReportSignalStoreCatalogRecord *replaced =
        source.find(input_record.sleep_day);
    const size_t count = source.record_count_ + (replaced ? 0 : 1);
    std::shared_ptr<ReportSignalStoreCatalog> catalog(
        new (std::nothrow) ReportSignalStoreCatalog());
    if (!catalog || !catalog->allocate(count)) return {};

    size_t source_index = 0;
    size_t output = 0;
    bool inserted = false;
    while (output < count) {
        while (source_index < source.record_count_ && replaced &&
               source.records_ + source_index == replaced) {
            ++source_index;
        }

        const bool use_input = !inserted &&
            (source_index >= source.record_count_ ||
             source.records_[source_index].sleep_day <
                 input_record.sleep_day);
        if (use_input) {
            catalog->records_[output] = std::move(input_record);
            inserted = true;
        } else {
            if (source_index >= source.record_count_) return {};
            catalog->records_[output] = source.records_[source_index++];
        }
        ++output;
    }
    return inserted ? catalog : nullptr;
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::reconcile(
    const ReportSignalStoreCatalog &source,
    const NightCatalog &night_catalog) {
    // Stale metadata retains generation history, but ready() checks revision.
    size_t count = 0;
    for (size_t i = 0; i < source.record_count_; ++i) {
        const ReportSignalStoreCatalogRecord &record = source.records_[i];
        const NightCatalogRecord *night = night_catalog.find(record.sleep_day);
        if (night) ++count;
    }

    std::shared_ptr<ReportSignalStoreCatalog> catalog(
        new (std::nothrow) ReportSignalStoreCatalog());
    if (!catalog || !catalog->allocate(count)) return {};

    size_t output = 0;
    for (size_t i = 0; i < source.record_count_; ++i) {
        const ReportSignalStoreCatalogRecord &record = source.records_[i];
        const NightCatalogRecord *night = night_catalog.find(record.sleep_day);
        if (!night) continue;

        catalog->records_[output++] = record;
    }
    return output == count ? catalog : nullptr;
}

}  // namespace aircannect
