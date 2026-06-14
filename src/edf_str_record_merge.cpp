#include "edf_str_record_merge.h"

#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_signal_table.h"
#include "edf_str_file_layout.h"

namespace aircannect {
namespace {

static constexpr int16_t EDF_STR_MISSING = -1;

struct MaskEvent {
    int16_t on = EDF_STR_MISSING;
    int16_t off = EDF_STR_MISSING;
};

bool valid_event_count(int16_t count) {
    return count >= 0 &&
           count <= static_cast<int16_t>(AC_EDF_STR_MASK_EVENT_CAPACITY);
}

bool valid_event(const MaskEvent &event) {
    return event.on >= 0 && event.on <= 1440 &&
           event.off >= 0 && event.off <= 1440 &&
           event.off >= event.on;
}

bool same_event(const MaskEvent &a, const MaskEvent &b) {
    return a.on == b.on && a.off == b.off;
}

bool append_unique_event(MaskEvent *events,
                         size_t &count,
                         const MaskEvent &event) {
    if (!valid_event(event)) return true;
    for (size_t i = 0; i < count; ++i) {
        if (same_event(events[i], event)) return true;
    }
    if (count >= AC_EDF_STR_MASK_EVENT_CAPACITY) return false;
    size_t insert = count;
    while (insert > 0) {
        const MaskEvent &prev = events[insert - 1];
        if (prev.on < event.on ||
            (prev.on == event.on && prev.off <= event.off)) {
            break;
        }
        events[insert] = prev;
        --insert;
    }
    events[insert] = event;
    ++count;
    return true;
}

EdfStrRecordMergeStatus collect_mask_events(const uint8_t *record,
                                            MaskEvent *events,
                                            size_t &count) {
    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL);
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL);
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const int16_t declared = edf_read_i16_le_sample(record, events_offset);
    if (declared == EDF_STR_MISSING) return EdfStrRecordMergeStatus::Ok;
    if (!valid_event_count(declared)) {
        return EdfStrRecordMergeStatus::BadMaskEvents;
    }

    for (int16_t i = 0; i < declared; ++i) {
        MaskEvent event;
        event.on = edf_read_i16_le_sample(record,
                                          on_offset + static_cast<size_t>(i));
        event.off = edf_read_i16_le_sample(record,
                                           off_offset + static_cast<size_t>(i));
        if (!valid_event(event)) {
            return EdfStrRecordMergeStatus::BadMaskEvents;
        }
        if (!append_unique_event(events, count, event)) {
            return EdfStrRecordMergeStatus::MaskEventOverflow;
        }
    }
    return EdfStrRecordMergeStatus::Ok;
}

bool mask_session_sample(size_t sample_offset) {
    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL);
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL);
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);

    return sample_offset == edf_str_signal_sample_offset(
                                AC_EDF_STR_DATE_SIGNAL) ||
           (sample_offset >= on_offset &&
            sample_offset < on_offset + AC_EDF_STR_MASK_EVENT_CAPACITY) ||
           (sample_offset >= off_offset &&
            sample_offset < off_offset + AC_EDF_STR_MASK_EVENT_CAPACITY) ||
           sample_offset == events_offset ||
           sample_offset == duration_offset;
}

bool summary_sample(size_t sample_offset) {
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::Summary) {
            continue;
        }
        if (edf_str_signal_sample_offset(i) == sample_offset) return true;
    }
    return false;
}

void merge_non_session_samples(const uint8_t *existing, uint8_t *incoming) {
    for (size_t i = 0; i < AC_EDF_STR_DATA_SAMPLES_PER_RECORD; ++i) {
        if (mask_session_sample(i)) continue;
        if (summary_sample(i)) continue;
        if (edf_read_i16_le_sample(incoming, i) == EDF_STR_MISSING) {
            const int16_t existing_value =
                edf_read_i16_le_sample(existing, i);
            if (existing_value != EDF_STR_MISSING) {
                edf_write_i16_le_sample(incoming, i, existing_value);
            }
        }
    }
}

void write_mask_events(uint8_t *record,
                       const MaskEvent *events,
                       size_t count) {
    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL);
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL);
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);

    for (size_t i = 0; i < AC_EDF_STR_MASK_EVENT_CAPACITY; ++i) {
        edf_write_i16_le_sample(record, on_offset + i, EDF_STR_MISSING);
        edf_write_i16_le_sample(record, off_offset + i, EDF_STR_MISSING);
    }

    int duration = 0;
    for (size_t i = 0; i < count; ++i) {
        edf_write_i16_le_sample(record, on_offset + i, events[i].on);
        edf_write_i16_le_sample(record, off_offset + i, events[i].off);
        duration += events[i].off > events[i].on
                        ? events[i].off - events[i].on
                        : 0;
    }
    if (duration > 1440) duration = 1440;
    edf_write_i16_le_sample(record, events_offset,
                            static_cast<int16_t>(count));
    edf_write_i16_le_sample(record,
                            duration_offset,
                            count > 0 ? static_cast<int16_t>(duration)
                                      : EDF_STR_MISSING);
}

void patch_crc(uint8_t *record) {
    const size_t data_bytes = AC_EDF_STR_DATA_SAMPLES_PER_RECORD * 2;
    const uint16_t crc = edf_crc16_ccitt_false(record, data_bytes);
    edf_write_i16_le_sample(
        record,
        edf_str_signal_sample_offset(AC_EDF_STR_CRC_SIGNAL),
        static_cast<int16_t>(crc));
}

}  // namespace

EdfStrRecordMergeStatus edf_str_merge_existing_record(
    const uint8_t *existing_record,
    size_t existing_len,
    uint8_t *incoming_record,
    size_t incoming_len) {
    if (!existing_record || !incoming_record) {
        return EdfStrRecordMergeStatus::InvalidArgument;
    }
    if (existing_len != edf_str_record_size() ||
        incoming_len != edf_str_record_size()) {
        return EdfStrRecordMergeStatus::RecordSizeMismatch;
    }

    const int16_t existing_date =
        edf_str_record_date_sample(existing_record, existing_len);
    const int16_t incoming_date =
        edf_str_record_date_sample(incoming_record, incoming_len);
    if (!edf_str_date_sample_valid(existing_date) ||
        !edf_str_date_sample_valid(incoming_date)) {
        return EdfStrRecordMergeStatus::BadDate;
    }
    if (existing_date != incoming_date) {
        return EdfStrRecordMergeStatus::DateMismatch;
    }

    MaskEvent events[AC_EDF_STR_MASK_EVENT_CAPACITY] = {};
    size_t event_count = 0;
    EdfStrRecordMergeStatus status =
        collect_mask_events(existing_record, events, event_count);
    if (status != EdfStrRecordMergeStatus::Ok) return status;
    status = collect_mask_events(incoming_record, events, event_count);
    if (status != EdfStrRecordMergeStatus::Ok) return status;

    merge_non_session_samples(existing_record, incoming_record);
    write_mask_events(incoming_record, events, event_count);
    patch_crc(incoming_record);
    return EdfStrRecordMergeStatus::Ok;
}

const char *edf_str_record_merge_status_name(
    EdfStrRecordMergeStatus status) {
    switch (status) {
        case EdfStrRecordMergeStatus::Ok: return "ok";
        case EdfStrRecordMergeStatus::InvalidArgument:
            return "invalid_argument";
        case EdfStrRecordMergeStatus::RecordSizeMismatch:
            return "record_size_mismatch";
        case EdfStrRecordMergeStatus::BadDate:
            return "bad_date";
        case EdfStrRecordMergeStatus::DateMismatch:
            return "date_mismatch";
        case EdfStrRecordMergeStatus::BadMaskEvents:
            return "bad_mask_events";
        case EdfStrRecordMergeStatus::MaskEventOverflow:
            return "mask_event_overflow";
        default:
            return "unknown";
    }
}

}  // namespace aircannect
