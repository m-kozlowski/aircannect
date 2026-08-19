#include "edf_signal_router.h"

#include "edf_stream_signal_table.h"

namespace aircannect {

bool edf_signal_target_for_stream(StreamSignalId id,
                                  EdfSignalTarget &target) {
    const EdfStreamSignalDescriptor *descriptor =
        edf_stream_signal_descriptor_for_stream(id);
    const EdfFileSchema *schema = descriptor
        ? edf_numeric_schema_for_series(descriptor->series)
        : nullptr;
    if (!descriptor || !schema || schema->source_samples_per_record == 0) {
        return false;
    }
    target = {descriptor->series,
              descriptor->source_index,
              static_cast<uint32_t>(
                  AC_EDF_RECORD_MS / schema->source_samples_per_record)};
    return true;
}

}  // namespace aircannect
