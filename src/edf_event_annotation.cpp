#include "edf_event_annotation.h"

#include "edf_time.h"

namespace aircannect {
namespace {

void set_result(EdfEventAnnotationResult &result,
                EdfEventAnnotationStatus status,
                const char *error) {
    result.status = status;
    result.error = error;
}

}  // namespace

bool edf_event_frame_is_respiratory(const As11EventFrame &frame) {
    return frame.data_id == "TherapyEvents-RespiratoryEvents";
}

bool edf_event_record_is_csr(const As11EventRecord &record) {
    return record.name == "CsrStart" || record.name == "CsrEnd";
}

bool edf_build_event_annotation(EdfAnnotationKind kind,
                                const As11EventRecord &record,
                                int64_t session_start_epoch_ms,
                                EdfAnnotationRecord &annotation,
                                EdfEventAnnotationResult &result) {
    EdfAnnotationRecord empty;
    annotation = empty;
    set_result(result, EdfEventAnnotationStatus::Ok, nullptr);

    const char *label = nullptr;
    if (!edf_annotation_label_for_event(kind, record.name.c_str(), label)) {
        set_result(result, EdfEventAnnotationStatus::UnsupportedEvent,
                   nullptr);
        return false;
    }

    int64_t event_ms = 0;
    if (session_start_epoch_ms <= 0 ||
        !edf_parse_utc_ms(record.report_time.c_str(), event_ms)) {
        set_result(result, EdfEventAnnotationStatus::TimeError,
                   "event_time_failed");
        return false;
    }

    int64_t onset_ms = event_ms - session_start_epoch_ms;
    if (onset_ms < 0) onset_ms = 0;

    int32_t duration_ms = 0;
    if (kind == EdfAnnotationKind::Eve &&
        record.has_duration && record.duration_ms > 0) {
        duration_ms = record.duration_ms;
    }

    annotation.onset_seconds = static_cast<int32_t>(onset_ms / 1000);
    annotation.duration_seconds =
        static_cast<int32_t>((duration_ms + 500) / 1000);
    annotation.label = label;
    return true;
}

}  // namespace aircannect
