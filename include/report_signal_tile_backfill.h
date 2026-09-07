#pragma once

#include <memory>

#include "report_signal_store.h"
#include "report_signal_store_catalog.h"
#include "report_signal_tile_writer.h"

namespace aircannect {

// Fills the persisted HTTP tiles for one already materialized night. The
// writer checks each tile header before reading and compressing its source
// range, so a resumed pass only does missing or stale work.
class ReportSignalTileBackfill {
public:
    ~ReportSignalTileBackfill();

    void begin(StorageReadPort &read, StorageRangeWritePort &write);
    bool start(std::shared_ptr<const LargeByteBuffer> metadata,
               uint32_t generation);
    bool poll();
    void reset();

    bool active() const { return active_; }
    bool succeeded() const { return succeeded_; }
    SleepDayId sleep_day() const { return night_.night.sleep_day; }

private:
    static constexpr size_t LevelCount = 3;

    bool start_next_level();
    bool level_supported(const ReportSignalStoreTrack &track,
                         ReportSignalStoreLevel level) const;

    ReportSignalTileWriter writer_;
    std::shared_ptr<const LargeByteBuffer> metadata_;
    ReportSignalStoreNightView night_;
    ReportSignalStoreTrack track_;
    size_t track_index_ = 0;
    size_t level_index_ = 0;
    uint32_t generation_ = 0;
    bool active_ = false;
    bool succeeded_ = true;
};

}  // namespace aircannect
