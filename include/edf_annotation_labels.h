#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_inventory.h"
#include "edf_file_writer.h"

namespace aircannect {

enum class EdfAnnotationLabelId : uint8_t {
    Hypopnea,
    CentralApnea,
    ObstructiveApnea,
    UnclassifiedApnea,
    Arousal,
    CsrStart,
    CsrEnd,
};

const char *edf_annotation_label_text(EdfAnnotationLabelId id);
bool edf_annotation_label_id_for_event(EdfAnnotationKind kind,
                                       const char *event_name,
                                       EdfAnnotationLabelId &out);
bool edf_annotation_label_id_for_text(EdfInventoryFileKind kind,
                                      const uint8_t *label,
                                      size_t len,
                                      EdfAnnotationLabelId &out);

}  // namespace aircannect
