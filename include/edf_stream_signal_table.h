#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "as11_stream_signals.h"
#include "edf_file_writer.h"
#include "edf_series.h"

namespace aircannect {

struct EdfStreamSignalDescriptor {
    const char *short_tag = "";
    StreamSignalId stream_id = StreamSignalId::Unknown;
    EdfSeriesId series = EdfSeriesId::Brp;
    uint8_t source_index = 0;
};

const EdfStreamSignalDescriptor *edf_stream_signal_descriptors(
    size_t &count);
const EdfStreamSignalDescriptor *edf_stream_signal_descriptor_for_stream(
    StreamSignalId id);
std::string edf_stream_ids_csv(bool required_only = false);
std::string edf_stream_ids_csv_excluding(EdfSeriesId excluded_series,
                                         bool required_only = false);

}  // namespace aircannect
