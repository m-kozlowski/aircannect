#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_writer.h"
#include "edf_storage_catalog.h"

namespace aircannect {

enum class EdfStrSessionStatus : uint8_t {
    Ok,
    BadSleepDay,
    MaskEventsFull,
    OffsetError,
};

class EdfStrSessionAccumulator {
public:
    void reset_day(uint16_t epoch_days,
                   const EdfLocalDateTime &sleep_day_start);
    bool begin_therapy(const EdfLocalDateTime &start,
                       EdfStrSessionStatus &status);
    bool finish_therapy(const EdfLocalDateTime &end,
                        bool &record_ready,
                        EdfStrSessionStatus &status);
    bool begin_mask_event(const EdfLocalDateTime &start,
                          EdfStrSessionStatus &status);
    bool finish_mask_event(const EdfLocalDateTime &end,
                           EdfStrSessionStatus &status);

    bool set_signal_physical(size_t signal_index, float physical_value);
    bool set_signal_digital(size_t signal_index, int16_t digital_value);

    bool active() const { return day_active_; }
    bool therapy_open() const { return therapy_open_; }
    bool mask_event_open() const { return mask_event_open_; }
    uint16_t day_epoch_days() const { return day_epoch_days_; }
    const EdfLocalDateTime &day_start() const { return day_start_; }
    const int16_t *samples() const { return samples_; }
    size_t sample_count() const { return AC_EDF_STR_DATA_SAMPLES_PER_RECORD; }
    uint8_t mask_events() const { return mask_events_; }

private:
    bool reset_offsets_valid() const;

    bool day_active_ = false;
    bool therapy_open_ = false;
    bool mask_event_open_ = false;
    uint16_t day_epoch_days_ = 0;
    uint16_t current_therapy_start_minute_ = 0;
    uint16_t current_mask_on_minute_ = 0;
    uint8_t mask_events_ = 0;
    EdfLocalDateTime day_start_;
    int16_t samples_[AC_EDF_STR_DATA_SAMPLES_PER_RECORD] = {};
};

}  // namespace aircannect
