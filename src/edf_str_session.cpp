#include "edf_str_session.h"

namespace aircannect {

namespace {

bool offset_valid(size_t signal_index) {
    return edf_str_signal_sample_offset(signal_index) <
           AC_EDF_STR_DATA_SAMPLES_PER_RECORD;
}

}  // namespace

bool EdfStrSessionAccumulator::reset_offsets_valid() const {
    return offset_valid(AC_EDF_STR_DATE_SIGNAL) &&
           offset_valid(AC_EDF_STR_MASK_EVENTS_SIGNAL) &&
           offset_valid(AC_EDF_STR_DURATION_SIGNAL);
}

void EdfStrSessionAccumulator::reset_day(
    uint16_t epoch_days,
    const EdfLocalDateTime &sleep_day_start) {
    for (size_t i = 0; i < AC_EDF_STR_DATA_SAMPLES_PER_RECORD; ++i) {
        samples_[i] = -1;
    }

    const size_t date_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DATE_SIGNAL);
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
    if (date_offset < AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        samples_[date_offset] = static_cast<int16_t>(epoch_days);
    }
    if (events_offset < AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        samples_[events_offset] = 0;
    }
    if (duration_offset < AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        samples_[duration_offset] = 0;
    }

    day_active_ = true;
    mask_open_ = false;
    day_epoch_days_ = epoch_days;
    day_start_ = sleep_day_start;
    current_on_minute_ = 0;
    mask_events_ = 0;
}

bool EdfStrSessionAccumulator::begin_session(
    const EdfLocalDateTime &start,
    EdfStrSessionStatus &status) {
    status = EdfStrSessionStatus::Ok;

    uint16_t day = 0;
    uint16_t minute = 0;
    EdfLocalDateTime sleep_day_start;
    if (!edf_sleep_day_epoch_days(start, day) ||
        !edf_sleep_day_minute(start, minute) ||
        !edf_sleep_day_start(start, sleep_day_start)) {
        status = EdfStrSessionStatus::BadSleepDay;
        return false;
    }

    if (!day_active_ || day_epoch_days_ != day) {
        reset_day(day, sleep_day_start);
    }

    if (!reset_offsets_valid()) {
        status = EdfStrSessionStatus::OffsetError;
        return false;
    }
    if (mask_events_ >= AC_EDF_STR_MASK_EVENT_CAPACITY) {
        status = EdfStrSessionStatus::MaskEventsFull;
        return false;
    }

    current_on_minute_ = minute;
    mask_open_ = true;
    return true;
}

bool EdfStrSessionAccumulator::finish_session(
    const EdfLocalDateTime &end,
    bool &record_ready,
    EdfStrSessionStatus &status) {
    record_ready = false;
    status = EdfStrSessionStatus::Ok;
    if (!day_active_ || !mask_open_) return true;

    uint16_t end_day = 0;
    uint16_t end_minute = 0;
    if (!edf_sleep_day_epoch_days(end, end_day) ||
        !edf_sleep_day_minute(end, end_minute)) {
        status = EdfStrSessionStatus::BadSleepDay;
        return false;
    }

    uint16_t off_minute = end_minute;
    if (end_day != day_epoch_days_) {
        off_minute = 1440;
    } else if (off_minute < current_on_minute_) {
        off_minute = current_on_minute_;
    }

    if (mask_events_ >= AC_EDF_STR_MASK_EVENT_CAPACITY) {
        status = EdfStrSessionStatus::MaskEventsFull;
        return false;
    }

    const size_t event_index = mask_events_;
    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL) +
        event_index;
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL) +
        event_index;
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
    if (on_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        off_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        events_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        duration_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        status = EdfStrSessionStatus::OffsetError;
        return false;
    }

    samples_[on_offset] = static_cast<int16_t>(current_on_minute_);
    samples_[off_offset] = static_cast<int16_t>(off_minute);
    mask_events_++;
    samples_[events_offset] = static_cast<int16_t>(mask_events_);

    int duration = samples_[duration_offset];
    if (duration < 0) duration = 0;
    duration += off_minute > current_on_minute_
                    ? off_minute - current_on_minute_
                    : 0;
    if (duration > 1440) duration = 1440;
    samples_[duration_offset] = static_cast<int16_t>(duration);
    mask_open_ = false;
    record_ready = true;
    return true;
}

bool EdfStrSessionAccumulator::set_signal_physical(size_t signal_index,
                                                   float physical_value) {
    const EdfSignalSpec *spec = edf_str_signal_spec(signal_index);
    if (!spec || spec->samples_per_record != 1) return false;
    const size_t offset = edf_str_signal_sample_offset(signal_index);
    if (offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) return false;
    samples_[offset] = edf_encode_physical_sample(*spec, physical_value);
    return true;
}

bool EdfStrSessionAccumulator::set_signal_digital(size_t signal_index,
                                                  int16_t digital_value) {
    const EdfSignalSpec *spec = edf_str_signal_spec(signal_index);
    if (!spec || spec->samples_per_record != 1) return false;
    const size_t offset = edf_str_signal_sample_offset(signal_index);
    if (offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) return false;
    samples_[offset] = digital_value;
    return true;
}

}  // namespace aircannect
