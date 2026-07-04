#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_durable_night_index.h"
#include "report_night_epoch_state.h"

namespace aircannect {

class ReportNightIndexRuntime {
public:
    bool begin();

    bool load_durable();
    bool seed_from_durable(ReportNightIndex &index) const;
    void schedule_durable_save(const ReportIndexedNight *src,
                               size_t count,
                               uint32_t content_crc) const;
    bool service_durable_writer();

    void clear_epochs();
    void note_chunk_committed(uint64_t night_start_ms);
    void remove_night(uint64_t night_start_ms);
    uint32_t data_epoch() const;
    void format_result_etag(const ReportIndexedNight &night,
                            char *out,
                            size_t out_size) const;

private:
    uint32_t night_epoch(uint64_t night_start_ms) const;

    ReportDurableNightIndex durable_;
    ReportNightEpochState epochs_;
};

}  // namespace aircannect
