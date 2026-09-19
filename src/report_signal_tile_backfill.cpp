#include "report_signal_tile_backfill.h"

#include <utility>

namespace aircannect {

ReportSignalTileBackfill::~ReportSignalTileBackfill() { reset(); }

void ReportSignalTileBackfill::begin(StorageReadPort &read,
                                     StorageRangeWritePort &write) {
    reset();
    writer_.begin(read, write);
}

bool ReportSignalTileBackfill::start(
    const ReportSignalStoreMetadata &metadata, uint32_t generation) {
    reset();
    if (!metadata.metadata || !generation) {
        succeeded_ = false;
        return false;
    }

    metadata_ = metadata.metadata;
    night_ = metadata.view;
    generation_ = generation;
    active_ = true;
    return true;
}

bool ReportSignalTileBackfill::level_supported(
    const ReportSignalStoreTrack &track,
    ReportSignalStoreLevel level) const {
    switch (level) {
        case ReportSignalStoreLevel::Raw:
            return true;
        case ReportSignalStoreLevel::OneSecond:
            return (track.lod_mask & REPORT_SIGNAL_STORE_LOD_1S) != 0;
        case ReportSignalStoreLevel::TenSeconds:
            return (track.lod_mask & REPORT_SIGNAL_STORE_LOD_10S) != 0;
    }
    return false;
}

bool ReportSignalTileBackfill::start_next_level() {
    static constexpr ReportSignalStoreLevel levels[LevelCount] = {
        ReportSignalStoreLevel::Raw,
        ReportSignalStoreLevel::OneSecond,
        ReportSignalStoreLevel::TenSeconds,
    };

    while (track_index_ < night_.night.track_count) {
        if (level_index_ >= LevelCount) {
            ++track_index_;
            level_index_ = 0;
            continue;
        }

        if (level_index_ == 0 && !night_.track(track_index_, track_)) {
            ++track_index_;
            level_index_ = 0;
            continue;
        }

        const ReportSignalStoreLevel level = levels[level_index_++];
        if (!level_supported(track_, level)) {
            continue;
        }

        StorageRangeWriteCommand memory;
        writer_.start(track_, level, 0, track_.block_slot_count,
                      std::move(memory), 0, generation_,
                      StorageAtomicWriteLane::Maintenance);
        if (!writer_.active() && !writer_.succeeded()) {
            succeeded_ = false;
            active_ = false;
            metadata_.reset();
            return true;
        }
        return true;
    }

    active_ = false;
    metadata_.reset();
    return true;
}

bool ReportSignalTileBackfill::poll() {
    if (!active_) return false;

    if (writer_.active()) {
        const bool worked = writer_.poll();
        if (writer_.active()) return worked;
        if (!writer_.succeeded()) {
            succeeded_ = false;
            active_ = false;
            metadata_.reset();
        }
        return true;
    }

    return start_next_level();
}

void ReportSignalTileBackfill::reset() {
    writer_.reset();
    metadata_.reset();
    night_ = {};
    track_ = {};
    track_index_ = 0;
    level_index_ = 0;
    generation_ = 0;
    active_ = false;
    succeeded_ = true;
}

}  // namespace aircannect
