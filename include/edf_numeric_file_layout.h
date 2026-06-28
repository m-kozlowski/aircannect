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

void edf_reset_numeric_file_layout(EdfNumericFileLayout &layout);

bool edf_accepted_data_ids_contain(const char *accepted_data_ids_csv,
                                   const char *data_id);
bool edf_short_tag_is_accepted(const char *accepted_data_ids_csv,
                               const char *short_tag);
bool edf_build_numeric_file_layout(EdfFileKind kind,
                                   const char *accepted_data_ids_csv,
                                   EdfNumericFileLayout &layout);

}  // namespace aircannect
