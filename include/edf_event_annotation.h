#pragma once

#include <stdint.h>

#include "as11_event_frame.h"
#include "edf_file_writer.h"

namespace aircannect {

enum class EdfEventAnnotationStatus : uint8_t {
    Ok,
    UnsupportedEvent,
    TimeError,
};

struct EdfEventAnnotationResult {
    EdfEventAnnotationStatus status = EdfEventAnnotationStatus::Ok;
    const char *error = nullptr;
};

bool edf_event_frame_is_respiratory(const As11EventFrame &frame);
bool edf_event_record_is_csr(const As11EventRecord &record);
bool edf_build_event_annotation(EdfAnnotationKind kind,
                                const As11EventRecord &record,
                                int64_t session_start_epoch_ms,
                                EdfAnnotationRecord &annotation,
                                EdfEventAnnotationResult &result);

}  // namespace aircannect
