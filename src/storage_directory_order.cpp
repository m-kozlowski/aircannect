#include "storage_directory_order.h"

#include <string.h>

namespace aircannect {

int storage_directory_entry_order(uint64_t modified_a,
                                  const char *name_a,
                                  uint64_t modified_b,
                                  const char *name_b) {
    if (modified_a != modified_b) return modified_a > modified_b ? -1 : 1;

    return strcmp(name_a ? name_a : "", name_b ? name_b : "");
}

}  // namespace aircannect
