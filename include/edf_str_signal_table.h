#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_writer.h"

namespace aircannect {

enum class EdfStrFieldSource : uint8_t {
    Custom,
    Derived,
    SettingGet,
    Summary,
};

struct EdfStrSignalDescriptor {
    EdfStrFieldSource source = EdfStrFieldSource::Custom;
    const char *short_tag = nullptr;
    EdfSignalSpec spec;
};

static constexpr size_t AC_EDF_STR_SIGNAL_COUNT = AC_EDF_STR_CRC_SIGNAL + 1;
static constexpr size_t AC_EDF_STR_SOURCE_FIELD_COUNT = AC_EDF_STR_CRC_SIGNAL;

const EdfStrSignalDescriptor *edf_str_signal_descriptor(size_t signal_index);

}  // namespace aircannect
