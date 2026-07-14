#pragma once

#include <stdint.h>

namespace aircannect {

int storage_directory_entry_order(uint64_t modified_a,
                                  const char *name_a,
                                  uint64_t modified_b,
                                  const char *name_b);

}  // namespace aircannect
