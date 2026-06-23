#include "edf_annotation_labels.h"

#include <string.h>

namespace aircannect {
namespace {

struct AnnotationEventMap {
    EdfAnnotationKind kind;
    const char *event_name;
    EdfAnnotationLabelId label;
};

struct AnnotationLabelMap {
    EdfInventoryFileKind kind;
    EdfAnnotationLabelId label;
    const char *text;
};

const AnnotationEventMap EVENT_MAP[] = {
    {EdfAnnotationKind::Eve, "HypopneaEnd", EdfAnnotationLabelId::Hypopnea},
    {EdfAnnotationKind::Eve,
     "CentralApneaEnd",
     EdfAnnotationLabelId::CentralApnea},
    {EdfAnnotationKind::Eve,
     "ObstructiveApneaEnd",
     EdfAnnotationLabelId::ObstructiveApnea},
    {EdfAnnotationKind::Eve,
     "ApneaEnd",
     EdfAnnotationLabelId::UnclassifiedApnea},
    {EdfAnnotationKind::Eve, "ReraEnd", EdfAnnotationLabelId::Arousal},
    {EdfAnnotationKind::Csl, "CsrStart", EdfAnnotationLabelId::CsrStart},
    {EdfAnnotationKind::Csl, "CsrEnd", EdfAnnotationLabelId::CsrEnd},
};

const AnnotationLabelMap LABEL_MAP[] = {
    {EdfInventoryFileKind::Eve, EdfAnnotationLabelId::Hypopnea, "Hypopnea"},
    {EdfInventoryFileKind::Eve,
     EdfAnnotationLabelId::CentralApnea,
     "Central Apnea"},
    {EdfInventoryFileKind::Eve,
     EdfAnnotationLabelId::ObstructiveApnea,
     "Obstructive Apnea"},
    {EdfInventoryFileKind::Eve,
     EdfAnnotationLabelId::UnclassifiedApnea,
     "Apnea"},
    {EdfInventoryFileKind::Eve, EdfAnnotationLabelId::Arousal, "Arousal"},
    {EdfInventoryFileKind::Csl, EdfAnnotationLabelId::CsrStart, "CSR Start"},
    {EdfInventoryFileKind::Csl, EdfAnnotationLabelId::CsrEnd, "CSR End"},
};

bool label_equals(const uint8_t *label, size_t len, const char *expected) {
    return label && expected && strlen(expected) == len &&
           memcmp(label, expected, len) == 0;
}

}  // namespace

const char *edf_annotation_label_text(EdfAnnotationLabelId id) {
    for (const AnnotationLabelMap &row : LABEL_MAP) {
        if (row.label == id) return row.text;
    }
    return nullptr;
}

bool edf_annotation_label_id_for_event(EdfAnnotationKind kind,
                                       const char *event_name,
                                       EdfAnnotationLabelId &out) {
    if (!event_name) return false;
    for (const AnnotationEventMap &row : EVENT_MAP) {
        if (row.kind == kind && strcmp(row.event_name, event_name) == 0) {
            out = row.label;
            return true;
        }
    }
    return false;
}

bool edf_annotation_label_id_for_text(EdfInventoryFileKind kind,
                                      const uint8_t *label,
                                      size_t len,
                                      EdfAnnotationLabelId &out) {
    for (const AnnotationLabelMap &row : LABEL_MAP) {
        if (row.kind == kind && label_equals(label, len, row.text)) {
            out = row.label;
            return true;
        }
    }
    return false;
}

}  // namespace aircannect
