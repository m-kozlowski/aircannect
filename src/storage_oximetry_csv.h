#pragma once

#include <stdint.h>
#include <stddef.h>
#include "oximetry_archive.h"

namespace aircannect::OximetryCsv {

// Storage worker only, with EDF writes taking precedence over each step.
bool begin(void (*wake)());
void set_mounted(bool mounted);
bool live_step(uint32_t now);
bool archive_step(uint32_t now);
bool release_media();
void reset_archive();

bool write_chunk(const char *path, size_t offset, const void *data, size_t size,
                 bool replace_partial = false);

}  // namespace aircannect::OximetryCsv
