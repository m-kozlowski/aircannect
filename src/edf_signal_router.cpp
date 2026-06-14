#include "edf_signal_router.h"

#include "edf_stream_signal_table.h"

namespace aircannect {

bool edf_signal_target_for_stream(StreamSignalId id,
                                  EdfSignalTarget &target) {
    const EdfStreamSignalDescriptor *descriptor =
        edf_stream_signal_descriptor_for_stream(id);
    if (!descriptor) return false;
    target = {descriptor->series,
              descriptor->source_index,
              descriptor->sample_ms};
    return true;
}

}  // namespace aircannect
