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

enum class SessionMergeKind {
    RewriteOrCumulative,
    DisjointPartialDay,
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
                                            size_t &count,
                                            size_t *source_count = nullptr) {
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
        if (source_count) (*source_count)++;
        if (!append_unique_event(events, count, event)) {
            return EdfStrRecordMergeStatus::MaskEventOverflow;
        }
    }
    return EdfStrRecordMergeStatus::Ok;
}

bool mask_event_sample(size_t sample_offset) {
    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL);
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL);
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);

    return sample_offset == edf_str_signal_sample_offset(
                                AC_EDF_STR_DATE_SIGNAL) ||
           (sample_offset >= on_offset &&
            sample_offset < on_offset + AC_EDF_STR_MASK_EVENT_CAPACITY) ||
           (sample_offset >= off_offset &&
            sample_offset < off_offset + AC_EDF_STR_MASK_EVENT_CAPACITY) ||
           sample_offset == events_offset;
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
        if (mask_event_sample(i)) continue;
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

void merge_duration_sample(const uint8_t *existing,
                           uint8_t *incoming,
                           SessionMergeKind merge_kind) {
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
    if (duration_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) return;

    const int16_t existing_value =
        edf_read_i16_le_sample(existing, duration_offset);
    const int16_t incoming_value =
        edf_read_i16_le_sample(incoming, duration_offset);

    if (incoming_value == EDF_STR_MISSING) {
        if (existing_value != EDF_STR_MISSING) {
            edf_write_i16_le_sample(incoming, duration_offset, existing_value);
        }
        return;
    }
    if (existing_value == EDF_STR_MISSING) return;

    int16_t merged_value = existing_value > incoming_value
                               ? existing_value
                               : incoming_value;
    if (merge_kind == SessionMergeKind::DisjointPartialDay) {
        const int sum = static_cast<int>(existing_value) +
                        static_cast<int>(incoming_value);
        merged_value = static_cast<int16_t>(sum > 1440 ? 1440 : sum);
    }
    edf_write_i16_le_sample(incoming, duration_offset, merged_value);
}

SessionMergeKind classify_session_merge(size_t existing_event_count,
                                        size_t incoming_event_count,
                                        size_t merged_event_count) {
    const size_t largest_source_count =
        existing_event_count > incoming_event_count
            ? existing_event_count
            : incoming_event_count;
    if (merged_event_count > largest_source_count) {
        return SessionMergeKind::DisjointPartialDay;
    }
    return SessionMergeKind::RewriteOrCumulative;
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

    for (size_t i = 0; i < AC_EDF_STR_MASK_EVENT_CAPACITY; ++i) {
        edf_write_i16_le_sample(record, on_offset + i, EDF_STR_MISSING);
        edf_write_i16_le_sample(record, off_offset + i, EDF_STR_MISSING);
    }

    for (size_t i = 0; i < count; ++i) {
        edf_write_i16_le_sample(record, on_offset + i, events[i].on);
        edf_write_i16_le_sample(record, off_offset + i, events[i].off);
    }
    edf_write_i16_le_sample(record, events_offset,
                            static_cast<int16_t>(count));
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
    size_t existing_event_count = 0;
    size_t incoming_event_count = 0;
    EdfStrRecordMergeStatus status =
        collect_mask_events(existing_record,
                            events,
                            event_count,
                            &existing_event_count);
    if (status != EdfStrRecordMergeStatus::Ok) return status;
    status = collect_mask_events(incoming_record,
                                 events,
                                 event_count,
                                 &incoming_event_count);
    if (status != EdfStrRecordMergeStatus::Ok) return status;

    const SessionMergeKind session_merge =
        classify_session_merge(existing_event_count,
                               incoming_event_count,
                               event_count);
    merge_non_session_samples(existing_record, incoming_record);
    merge_duration_sample(existing_record, incoming_record, session_merge);
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
