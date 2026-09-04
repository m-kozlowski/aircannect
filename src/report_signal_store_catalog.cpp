#include "report_signal_store_catalog.h"

#include <new>
#include <utility>

#include "checked_size.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

bool input_identity(const ReportSignalStoreCatalogInput &input,
                    ReportSignalStoreNightView &view) {
    view = {};
    return input.metadata &&
           ReportSignalStoreNightCodec::decode(
               input.metadata->data(), input.metadata->size(), view);
}

bool fill_record(ReportSignalStoreCatalogRecord &record,
                 const ReportSignalStoreCatalogInput &input) {
    ReportSignalStoreNightView view;
    if (!input_identity(input, view)) return false;

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

    for (size_t i = 0; i < input_count; ++i) {
        ReportSignalStoreNightView view;
        if (!input_identity(inputs[i], view)) return {};

        for (size_t n = 0; n < i; ++n) {
            ReportSignalStoreNightView other;
            if (!input_identity(inputs[n], other) ||
                other.night.sleep_day == view.night.sleep_day) {
                return {};
            }
        }
    }

    std::shared_ptr<ReportSignalStoreCatalog> catalog(
        new (std::nothrow) ReportSignalStoreCatalog());
    if (!catalog || !catalog->allocate(input_count)) return {};

    SleepDayId previous;
    for (size_t output = 0; output < input_count; ++output) {
        size_t selected = input_count;
        ReportSignalStoreNightView selected_view;
        for (size_t candidate = 0; candidate < input_count; ++candidate) {
            ReportSignalStoreNightView view;
            if (!input_identity(inputs[candidate], view)) return {};
            if (output > 0 && !(view.night.sleep_day < previous)) continue;
            if (selected == input_count ||
                selected_view.night.sleep_day < view.night.sleep_day) {
                selected = candidate;
                selected_view = view;
            }
        }
        if (selected == input_count ||
            !fill_record(catalog->records_[output], inputs[selected])) {
            return {};
        }
        previous = catalog->records_[output].sleep_day;
    }
    return catalog;
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogBuilder::upsert(
    const ReportSignalStoreCatalog &source,
    const ReportSignalStoreCatalogInput &input) {
    ReportSignalStoreNightView input_view;
    if (!input_identity(input, input_view)) return {};

    const ReportSignalStoreCatalogRecord *replaced =
        source.find(input_view.night.sleep_day);
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
                 input_view.night.sleep_day);
        if (use_input) {
            if (!fill_record(catalog->records_[output], input)) return {};
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
    size_t count = 0;
    for (size_t i = 0; i < source.record_count_; ++i) {
        const ReportSignalStoreCatalogRecord &record = source.records_[i];
        const NightCatalogRecord *night = night_catalog.find(record.sleep_day);
        if (night && night->source_revision == record.source_revision) ++count;
    }

    std::shared_ptr<ReportSignalStoreCatalog> catalog(
        new (std::nothrow) ReportSignalStoreCatalog());
    if (!catalog || !catalog->allocate(count)) return {};

    size_t output = 0;
    for (size_t i = 0; i < source.record_count_; ++i) {
        const ReportSignalStoreCatalogRecord &record = source.records_[i];
        const NightCatalogRecord *night = night_catalog.find(record.sleep_day);
        if (!night || night->source_revision != record.source_revision) {
            continue;
        }
        catalog->records_[output++] = record;
    }
    return output == count ? catalog : nullptr;
}

}  // namespace aircannect
