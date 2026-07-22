#include "report_artifact_index.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "report_range_tile.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void *allocate_large(size_t size) {
    if (size == 0) return nullptr;
#ifdef ARDUINO
    return Memory::calloc_large(1, size, false);
#else
    return calloc(1, size);
#endif
}

void free_large(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

bool add_size(size_t &value, size_t add) {
    if (value > std::numeric_limits<size_t>::max() - add) return false;
    value += add;
    return true;
}

bool multiply_size(size_t count, size_t width, size_t &out) {
    if (width != 0 && count > std::numeric_limits<size_t>::max() / width) {
        return false;
    }
    out = count * width;
    return true;
}

bool align_size(size_t value, size_t alignment, size_t &out) {
    if (alignment == 0) return false;

    const size_t remainder = value % alignment;
    if (remainder == 0) {
        out = value;
        return true;
    }
    if (!add_size(value, alignment - remainder)) return false;
    out = value;
    return true;
}

bool valid_tile(const ReportRangeTileArtifact &tile) {
    return tile.start_ms > 0 &&
           tile.start_ms % REPORT_RANGE_TILE_MS == 0 &&
           tile.start_ms <= INT64_MAX - REPORT_RANGE_TILE_MS &&
           tile.end_ms == tile.start_ms + REPORT_RANGE_TILE_MS &&
           tile.size > 0 && tile.size <= UINT32_MAX;
}

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
        if (!valid_tile(tile) ||
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
    out.overview.key = ReportArtifactKey::overview(record.sleep_day,
                                                    record.source_revision);
    out.overview.size = record.overview_size;
    out.overview.crc32 = record.overview_crc32;
}

}  // namespace

ReportArtifactIndex::~ReportArtifactIndex() {
    free_large(storage_);
}

bool ReportArtifactIndex::allocate(size_t record_count, size_t tile_count) {
    size_t record_bytes = 0;
    size_t tile_offset = 0;
    size_t tile_bytes = 0;
    size_t total_bytes = 0;
    if (!multiply_size(record_count,
                       sizeof(ReportArtifactIndexRecord),
                       record_bytes) ||
        !align_size(record_bytes,
                    alignof(ReportRangeTileArtifact),
                    tile_offset) ||
        !multiply_size(tile_count,
                       sizeof(ReportRangeTileArtifact),
                       tile_bytes) ||
        tile_count > UINT32_MAX ||
        !add_size(total_bytes, tile_offset) ||
        !add_size(total_bytes, tile_bytes)) {
        return false;
    }

    if (total_bytes > 0) {
        storage_ = static_cast<uint8_t *>(allocate_large(total_bytes));
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
    ReportArtifactAvailability &out) const {
    out = {};
    if (!request.valid()) return false;

    const ReportArtifactIndexRecord *found = find(request.sleep_day);
    if (!found || found->source_revision != request.source_revision) {
        return false;
    }

    fill_availability(*found, request, out);
    if (request.kind == ReportArtifactKind::RangeTile) {
        size_t tile_count = 0;
        const ReportRangeTileArtifact *indexed_tiles = tiles(*found,
                                                             tile_count);
        for (size_t i = 0; indexed_tiles && i < tile_count; ++i) {
            const ReportRangeTileArtifact &tile = indexed_tiles[i];
            if (tile.start_ms != request.range_start_ms ||
                tile.end_ms != request.range_end_ms) {
                continue;
            }

            out.range_tile.key = request;
            out.range_tile.size = tile.size;
            out.range_tile.crc32 = tile.crc32;
            break;
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
            !add_size(tile_count, inputs[i].tile_count)) {
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
    if (!add_size(tile_count, input.tile_count)) return {};

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
        bool replacing = false;
        for (size_t i = 0; i < existing_tile_count; ++i) {
            if (existing_tiles[i].start_ms ==
                    availability.request.range_start_ms &&
                existing_tiles[i].end_ms ==
                    availability.request.range_end_ms) {
                replacing = true;
                break;
            }
        }
        if (!replacing) ++merged_tile_count;
        if (merged_tile_count > ReportArtifactManifestCodec::MaxTiles) {
            return {};
        }

        merged_tiles = static_cast<ReportRangeTileArtifact *>(allocate_large(
            merged_tile_count * sizeof(ReportRangeTileArtifact)));
        if (!merged_tiles) return {};

        const ReportRangeTileArtifact added{
            availability.request.range_start_ms,
            availability.request.range_end_ms,
            availability.range_tile.size,
            availability.range_tile.crc32,
        };
        size_t source_tile = 0;
        bool inserted = false;
        for (size_t output_tile = 0;
             output_tile < merged_tile_count;
             ++output_tile) {
            if (!inserted &&
                (source_tile >= existing_tile_count ||
                 added.start_ms <= existing_tiles[source_tile].start_ms)) {
                merged_tiles[output_tile] = added;
                if (source_tile < existing_tile_count &&
                    existing_tiles[source_tile].start_ms == added.start_ms &&
                    existing_tiles[source_tile].end_ms == added.end_ms) {
                    ++source_tile;
                }
                inserted = true;
            } else {
                merged_tiles[output_tile] = existing_tiles[source_tile++];
            }
        }
    }

    ReportArtifactIndexInput input;
    input.key = ReportArtifactKey::result(
        availability.request.sleep_day,
        availability.request.source_revision);
    input.result_size = availability.result.size;
    input.overview_size = availability.overview.size;
    input.result_crc32 = availability.result.crc32;
    input.overview_crc32 = availability.overview.crc32;
    input.tiles = merged_tiles ? merged_tiles : existing_tiles;
    input.tile_count = merged_tile_count;
    std::shared_ptr<const ReportArtifactIndex> merged =
        replace_input(source, input);
    free_large(merged_tiles);
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
        if (!add_size(tile_count, record.tile_count)) return {};
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
