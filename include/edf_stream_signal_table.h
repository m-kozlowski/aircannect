#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_stream_signals.h"
#include "edf_file_writer.h"
#include "edf_series.h"

namespace aircannect {

struct EdfStreamSignalDescriptor {
    const char *short_tag = "";
    StreamSignalId stream_id = StreamSignalId::Unknown;
    EdfFileKind file_kind = EdfFileKind::Brp;
    EdfSeriesId series = EdfSeriesId::Brp;
    uint8_t source_index = 0;
    uint32_t sample_ms = 0;
};

extern const char *const DEFAULT_EDF_STREAM_IDS;

const EdfStreamSignalDescriptor *edf_stream_signal_descriptors(
    size_t &count);
const EdfStreamSignalDescriptor *edf_stream_signal_descriptor_for_stream(
    StreamSignalId id);

}  // namespace aircannect
