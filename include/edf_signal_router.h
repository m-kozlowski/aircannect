#pragma once

#include <stdint.h>

#include "as11_stream_signals.h"
#include "edf_series.h"

namespace aircannect {

struct EdfSignalTarget {
    EdfSeriesId series = EdfSeriesId::Brp;
    uint8_t signal_index = 0;
    uint32_t sample_ms = 0;
};

bool edf_signal_target_for_stream(StreamSignalId id,
                                  EdfSignalTarget &target);

}  // namespace aircannect
