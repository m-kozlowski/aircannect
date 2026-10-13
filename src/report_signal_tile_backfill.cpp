#include "report_signal_tile_backfill.h"

#include <utility>

#include "string_util.h"

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
        remember_error("report_signal_tile_invalid_metadata");
        return false;
    }

    metadata_ = metadata.metadata;
    night_ = metadata.view;
    generation_ = generation;
    active_ = true;
    return true;
}

bool ReportSignalTileBackfill::start(
    std::shared_ptr<const ReportSignalStoreBundle> build,
    uint32_t generation, StorageAtomicWriteLane lane) {
    if (!build || !start({build->metadata, build->metadata_view}, generation)) {
        return false;
    }

    build_ = std::move(build);
    lane_ = lane;
    return true;
}

void ReportSignalTileBackfill::remember_error(const char *error) {
    if (error_[0] || !error || !error[0]) return;

    copy_cstr(error_, sizeof(error_), error);
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

        const auto *changed = build_ ? build_->signal(track_index_) : nullptr;
        if (level_index_ == 0) {
            if (build_) {
                if (!changed || !changed->changed_block_count) {
                    ++track_index_;
                    continue;
                }
                track_ = changed->track;
            } else if (!night_.track(track_index_, track_)) {
                ++track_index_;
                continue;
            }
        }

        const ReportSignalStoreLevel level = levels[level_index_++];
        if (!level_supported(track_, level)) {
            continue;
        }

        StorageRangeWriteCommand memory;
        writer_.start(track_, level,
                      changed ? changed->first_changed_slot : 0,
                      changed ? changed->changed_block_count : track_.block_slot_count,
                      std::move(memory), 0, generation_, lane_, build_ != nullptr);
        if (!writer_.active() && !writer_.succeeded()) {
            succeeded_ = false;
            remember_error(writer_.error());
            active_ = false;
            metadata_.reset();
            build_.reset();
            return true;
        }
        return true;
    }

    active_ = false;
    metadata_.reset();
    build_.reset();
    return true;
}

bool ReportSignalTileBackfill::poll() {
    if (!active_) return false;

    if (writer_.active()) {
        const bool worked = writer_.poll();
        if (writer_.active()) return worked;
        if (!writer_.succeeded()) {
            succeeded_ = false;
            remember_error(writer_.error());
            active_ = false;
            metadata_.reset();
            build_.reset();
        }
        return true;
    }

    return start_next_level();
}

void ReportSignalTileBackfill::reset() {
    writer_.reset();
    metadata_.reset();
    build_.reset();
    night_ = {};
    track_ = {};
    track_index_ = 0;
    level_index_ = 0;
    generation_ = 0;
    lane_ = StorageAtomicWriteLane::Maintenance;
    active_ = false;
    succeeded_ = true;
    error_[0] = '\0';
}

}  // namespace aircannect
