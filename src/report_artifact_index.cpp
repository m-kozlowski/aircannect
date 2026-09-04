#include "report_artifact_index.h"

#include <algorithm>
#include <new>
#include <string.h>

#include "checked_size.h"
#include "memory_manager.h"
#include "report_range_tile.h"

namespace aircannect {
namespace {

bool tile_follows(const ReportRangeTileArtifact &previous,
                  const ReportRangeTileArtifact &next) {
    return next.start_ms >= previous.end_ms;
}

bool valid_input(const ReportArtifactIndexInput &input) {
    if (input.key.kind != ReportArtifactKind::Result ||
        !input.key.valid() || input.result_size == 0 ||
        input.result_size > UINT32_MAX || input.overview_size == 0 ||
        input.overview_size > UINT32_MAX || input.tile_count > UINT16_MAX ||
        (input.tile_count > 0 && !input.tiles)) {
        return false;
    }

    ReportRangeTileArtifact previous;
    for (size_t i = 0; i < input.tile_count; ++i) {
        const ReportRangeTileArtifact &tile = input.tiles[i];
        if (!report_range_tile_artifact_valid(tile) ||
            (i > 0 && !tile_follows(previous, tile))) {
            return false;
        }
        previous = tile;
    }
    return true;
}

void fill_availability(const ReportArtifactIndexRecord &record,
                       const ReportArtifactKey &request,
                       ReportArtifactAvailability &out) {
    out = {};
    out.request = request;
    out.result.key = ReportArtifactKey::result(record.sleep_day,
                                                record.source_revision);
    out.result.size = record.result_size;
    out.result.crc32 = record.result_crc32;
    out.result.manifest_modified = record.manifest_modified;
    out.overview.key = ReportArtifactKey::overview(record.sleep_day,
                                                    record.source_revision);
    out.overview.size = record.overview_size;
    out.overview.crc32 = record.overview_crc32;
    out.overview.prefix_crc32 = record.overview_prefix_crc32;
    out.overview.manifest_modified = record.manifest_modified;
}

}  // namespace

ReportArtifactIndex::~ReportArtifactIndex() {
    Memory::free(storage_);
}

bool ReportArtifactIndex::allocate(size_t record_count, size_t tile_count) {
    size_t record_bytes = 0;
    size_t tile_offset = 0;
    size_t tile_bytes = 0;
    size_t total_bytes = 0;
    if (!CheckedSize::multiply(record_count,
                               sizeof(ReportArtifactIndexRecord),
                               record_bytes) ||
        !CheckedSize::align_up(record_bytes,
                               alignof(ReportRangeTileArtifact),
                               tile_offset) ||
        !CheckedSize::multiply(tile_count,
                               sizeof(ReportRangeTileArtifact),
                               tile_bytes) ||
        tile_count > UINT32_MAX ||
        !CheckedSize::add_to(total_bytes, tile_offset) ||
        !CheckedSize::add_to(total_bytes, tile_bytes)) {
        return false;
    }

    if (total_bytes > 0) {
        storage_ = static_cast<uint8_t *>(
            Memory::calloc_large(1, total_bytes, false));
        if (!storage_) return false;
    }

    storage_bytes_ = total_bytes;
    records_ = reinterpret_cast<ReportArtifactIndexRecord *>(storage_);
    tiles_ = reinterpret_cast<ReportRangeTileArtifact *>(
        storage_ ? storage_ + tile_offset : nullptr);
    record_count_ = record_count;
    tile_count_ = tile_count;
    return true;
}

const ReportArtifactIndexRecord *ReportArtifactIndex::find(
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

const ReportRangeTileArtifact *ReportArtifactIndex::tiles(
    const ReportArtifactIndexRecord &value,
    size_t &count) const {
    count = 0;
    if (!tiles_ || value.tile_count == 0 ||
        value.tile_offset > tile_count_ ||
        value.tile_count > tile_count_ - value.tile_offset) {
        return nullptr;
    }

    count = value.tile_count;
    return tiles_ + value.tile_offset;
}

bool ReportArtifactIndex::availability(
    const ReportArtifactKey &request,
    ReportArtifactAvailability &out,
    uint8_t requested_tile_count) const {
    out = {};
    if (!request.valid() ||
        !report_artifact_batch_count_valid(
            request.kind, requested_tile_count)) {
        return false;
    }

    const ReportArtifactIndexRecord *found = find(request.sleep_day);
    if (!found || found->source_revision != request.source_revision) {
        return false;
    }

    fill_availability(*found, request, out);
    out.requested_range_tile_count = requested_tile_count;
    if (request.kind == ReportArtifactKind::RangeTile) {
        size_t tile_count = 0;
        const ReportRangeTileArtifact *indexed_tiles = tiles(*found,
                                                             tile_count);
        for (size_t requested_index = 0;
             requested_index < requested_tile_count;
             ++requested_index) {
            const int64_t offset =
                static_cast<int64_t>(requested_index) *
                REPORT_RANGE_TILE_MS;
            for (size_t i = 0; indexed_tiles && i < tile_count; ++i) {
                const ReportRangeTileArtifact &tile = indexed_tiles[i];
                if (tile.start_ms != request.range_start_ms + offset ||
                    tile.end_ms != request.range_end_ms + offset) {
                    continue;
                }

                ReportArtifactDescriptor &descriptor =
                    out.range_tiles[out.range_tile_count++];
                descriptor.key = ReportArtifactKey::range_tile(
                    request.sleep_day,
                    request.source_revision,
                    tile.start_ms,
                    tile.end_ms);
                descriptor.size = tile.size;
                descriptor.crc32 = tile.crc32;
                descriptor.prefix_crc32 = tile.prefix_crc32;
                descriptor.manifest_modified = found->manifest_modified;
                break;
            }
        }
    }

    if (!out.requested_ready()) {
        out = {};
        return false;
    }
    return true;
}

std::shared_ptr<const ReportArtifactIndex> ReportArtifactIndexBuilder::build(
    const ReportArtifactIndexInput *inputs,
    size_t input_count) {
    if (input_count > 0 && !inputs) return {};

    size_t tile_count = 0;
    for (size_t i = 0; i < input_count; ++i) {
        if (!valid_input(inputs[i]) ||
            !CheckedSize::add_to(tile_count, inputs[i].tile_count)) {
            return {};
        }
        for (size_t j = 0; j < i; ++j) {
            if (inputs[j].key.sleep_day == inputs[i].key.sleep_day) {
                return {};
            }
        }
    }

    std::shared_ptr<ReportArtifactIndex> index(
        new (std::nothrow) ReportArtifactIndex());
    if (!index || !index->allocate(input_count, tile_count)) return {};

    size_t next_tile = 0;
    SleepDayId previous_day;
    for (size_t output = 0; output < input_count; ++output) {
        size_t selected = input_count;
        for (size_t candidate = 0; candidate < input_count; ++candidate) {
            const SleepDayId day = inputs[candidate].key.sleep_day;
            if (output > 0 && !(day < previous_day)) continue;
            if (selected == input_count ||
                inputs[selected].key.sleep_day < day) {
                selected = candidate;
            }
        }
        if (selected == input_count) return {};

        const ReportArtifactIndexInput &input = inputs[selected];
        ReportArtifactIndexRecord &record = index->records_[output];
        record.sleep_day = input.key.sleep_day;
        record.source_revision = input.key.source_revision;
        record.result_size = input.result_size;
        record.overview_size = input.overview_size;
        record.result_crc32 = input.result_crc32;
        record.overview_crc32 = input.overview_crc32;
        record.overview_prefix_crc32 = input.overview_prefix_crc32;
        record.manifest_modified = input.manifest_modified;
        record.tile_offset = static_cast<uint32_t>(next_tile);
        record.tile_count = static_cast<uint16_t>(input.tile_count);
        if (input.tile_count > 0) {
            memcpy(index->tiles_ + next_tile,
                   input.tiles,
                   input.tile_count * sizeof(*input.tiles));
            next_tile += input.tile_count;
        }
        previous_day = record.sleep_day;
    }

    return next_tile == tile_count ? index : nullptr;
}

std::shared_ptr<const ReportArtifactIndex>
ReportArtifactIndexBuilder::replace_input(
    const ReportArtifactIndex &source,
    const ReportArtifactIndexInput &input) {
    if (!valid_input(input)) return {};

    const ReportArtifactIndexRecord *replaced =
        source.find(input.key.sleep_day);
    const size_t record_count = source.record_count_ + (replaced ? 0 : 1);
    size_t tile_count = source.tile_count_;
    if (replaced) tile_count -= replaced->tile_count;
    if (!CheckedSize::add_to(tile_count, input.tile_count)) return {};

    std::shared_ptr<ReportArtifactIndex> index(
        new (std::nothrow) ReportArtifactIndex());
    if (!index || !index->allocate(record_count, tile_count)) return {};

    size_t source_record = 0;
    size_t output_record = 0;
    size_t output_tile = 0;
    bool inserted = false;
    while (output_record < record_count) {
        const ReportArtifactIndexRecord *old_record =
            source_record < source.record_count_
                ? source.records_ + source_record
                : nullptr;
        if (old_record && old_record->sleep_day == input.key.sleep_day) {
            ++source_record;
            old_record = source_record < source.record_count_
                ? source.records_ + source_record
                : nullptr;
        }

        const bool insert_replacement = !inserted &&
            (!old_record || old_record->sleep_day < input.key.sleep_day);
        ReportArtifactIndexRecord &out = index->records_[output_record++];
        out.tile_offset = static_cast<uint32_t>(output_tile);

        if (insert_replacement) {
            out.sleep_day = input.key.sleep_day;
            out.source_revision = input.key.source_revision;
            out.result_size = input.result_size;
            out.overview_size = input.overview_size;
            out.result_crc32 = input.result_crc32;
            out.overview_crc32 = input.overview_crc32;
            out.overview_prefix_crc32 = input.overview_prefix_crc32;
            out.manifest_modified = input.manifest_modified;
            out.tile_count = static_cast<uint16_t>(input.tile_count);
            if (input.tile_count > 0) {
                memcpy(index->tiles_ + output_tile,
                       input.tiles,
                       input.tile_count * sizeof(*input.tiles));
                output_tile += input.tile_count;
            }
            inserted = true;
            continue;
        }

        if (!old_record) return {};
        out = *old_record;
        out.tile_offset = static_cast<uint32_t>(output_tile);
        size_t old_tile_count = 0;
        const ReportRangeTileArtifact *old_tiles = source.tiles(
            *old_record, old_tile_count);
        if (old_tile_count > 0 && !old_tiles) return {};
        if (old_tile_count > 0) {
            memcpy(index->tiles_ + output_tile,
                   old_tiles,
                   old_tile_count * sizeof(*old_tiles));
            output_tile += old_tile_count;
        }
        ++source_record;
    }

    return inserted && source_record == source.record_count_ &&
                   output_tile == tile_count
        ? index
        : nullptr;
}

std::shared_ptr<const ReportArtifactIndex>
ReportArtifactIndexBuilder::merge_availability(
    const ReportArtifactIndex &source,
    const ReportArtifactAvailability &availability) {
    if (!availability.requested_ready()) return {};

    const ReportArtifactIndexRecord *existing =
        source.find(availability.request.sleep_day);
    size_t existing_tile_count = 0;
    const ReportRangeTileArtifact *existing_tiles = existing &&
            existing->source_revision ==
                availability.request.source_revision
        ? source.tiles(*existing, existing_tile_count)
        : nullptr;
    if (existing_tile_count > 0 && !existing_tiles) return {};

    ReportRangeTileArtifact *merged_tiles = nullptr;
    size_t merged_tile_count = existing_tile_count;
    if (availability.request.kind == ReportArtifactKind::RangeTile) {
        if (availability.range_tile_count == 0) return {};
        if (!CheckedSize::add_to(
                merged_tile_count, availability.range_tile_count)) {
            return {};
        }

        merged_tiles = static_cast<ReportRangeTileArtifact *>(
            Memory::calloc_large(merged_tile_count,
                                 sizeof(ReportRangeTileArtifact),
                                 false));
        if (!merged_tiles) return {};

        if (existing_tile_count > 0) {
            memcpy(merged_tiles,
                   existing_tiles,
                   existing_tile_count * sizeof(*existing_tiles));
        }

        for (size_t incoming = 0;
             incoming < availability.range_tile_count;
             ++incoming) {
            const ReportArtifactDescriptor *descriptor =
                availability.range_tile(incoming);
            if (!descriptor) {
                Memory::free(merged_tiles);
                return {};
            }

            ReportRangeTileArtifact added{
                descriptor->key.range_start_ms,
                descriptor->key.range_end_ms,
                descriptor->size,
                descriptor->crc32,
                descriptor->prefix_crc32,
            };
            bool replaced = false;
            for (size_t i = 0; i < existing_tile_count; ++i) {
                if (merged_tiles[i].start_ms == added.start_ms &&
                    merged_tiles[i].end_ms == added.end_ms) {
                    merged_tiles[i] = added;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) merged_tiles[existing_tile_count++] = added;
        }

        merged_tile_count = existing_tile_count;
        if (merged_tile_count > ReportArtifactManifestCodec::MaxTiles) {
            Memory::free(merged_tiles);
            return {};
        }
        std::sort(merged_tiles,
                  merged_tiles + merged_tile_count,
                  [](const ReportRangeTileArtifact &lhs,
                     const ReportRangeTileArtifact &rhs) {
                      return lhs.start_ms < rhs.start_ms;
                  });
    }

    ReportArtifactIndexInput input;
    input.key = ReportArtifactKey::result(
        availability.request.sleep_day,
        availability.request.source_revision);
    input.result_size = availability.result.size;
    input.overview_size = availability.overview.size;
    input.result_crc32 = availability.result.crc32;
    input.overview_crc32 = availability.overview.crc32;
    input.overview_prefix_crc32 = availability.overview.prefix_crc32;
    input.manifest_modified = availability.result.manifest_modified;
    input.tiles = merged_tiles ? merged_tiles : existing_tiles;
    input.tile_count = merged_tile_count;
    std::shared_ptr<const ReportArtifactIndex> merged =
        replace_input(source, input);
    Memory::free(merged_tiles);
    return merged;
}

std::shared_ptr<const ReportArtifactIndex>
ReportArtifactIndexBuilder::reconcile(const ReportArtifactIndex &source,
                                      const NightCatalog &catalog) {
    size_t record_count = 0;
    size_t tile_count = 0;
    for (size_t i = 0; i < source.record_count_; ++i) {
        const ReportArtifactIndexRecord &record = source.records_[i];
        const NightCatalogRecord *night = catalog.find(record.sleep_day);
        if (!night || night->source_revision != record.source_revision) {
            continue;
        }
        ++record_count;
        if (!CheckedSize::add_to(tile_count, record.tile_count)) return {};
    }

    std::shared_ptr<ReportArtifactIndex> index(
        new (std::nothrow) ReportArtifactIndex());
    if (!index || !index->allocate(record_count, tile_count)) return {};

    size_t output_record = 0;
    size_t output_tile = 0;
    for (size_t i = 0; i < source.record_count_; ++i) {
        const ReportArtifactIndexRecord &record = source.records_[i];
        const NightCatalogRecord *night = catalog.find(record.sleep_day);
        if (!night || night->source_revision != record.source_revision) {
            continue;
        }

        ReportArtifactIndexRecord &out = index->records_[output_record++];
        out = record;
        out.tile_offset = static_cast<uint32_t>(output_tile);
        size_t count = 0;
        const ReportRangeTileArtifact *record_tiles = source.tiles(record,
                                                                   count);
        if (count > 0 && !record_tiles) return {};
        if (count > 0) {
            memcpy(index->tiles_ + output_tile,
                   record_tiles,
                   count * sizeof(*record_tiles));
            output_tile += count;
        }
    }

    return output_record == record_count && output_tile == tile_count
        ? index
        : nullptr;
}

}  // namespace aircannect
