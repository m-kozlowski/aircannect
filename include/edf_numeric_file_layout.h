#pragma once

#include <stddef.h>

#include "edf_file_writer.h"

namespace aircannect {

struct EdfNumericFileLayout {
    bool enabled = false;
    EdfSignalSpec signals[AC_EDF_NUMERIC_SIGNAL_MAX + 1] = {};
    uint8_t source_indices[AC_EDF_NUMERIC_SIGNAL_MAX] = {};
    EdfFileSchema schema;
};

struct EdfNumericFileLayoutSet {
    EdfNumericFileLayout brp;
    EdfNumericFileLayout pld;
    EdfNumericFileLayout sa2;
};

void edf_reset_numeric_file_layout(EdfNumericFileLayout &layout);
void edf_reset_numeric_file_layout_set(EdfNumericFileLayoutSet &layouts);

bool edf_accepted_data_ids_contain(const char *accepted_data_ids_csv,
                                   const char *data_id);
bool edf_short_tag_is_accepted(const char *accepted_data_ids_csv,
                               const char *short_tag);
bool edf_build_numeric_file_layout(EdfFileKind kind,
                                   const char *accepted_data_ids_csv,
                                   EdfNumericFileLayout &layout);
bool edf_build_numeric_file_layout_set(const char *accepted_data_ids_csv,
                                       EdfNumericFileLayoutSet &layouts);
bool edf_numeric_file_layout_set_enabled(
    const EdfNumericFileLayoutSet &layouts);

}  // namespace aircannect
