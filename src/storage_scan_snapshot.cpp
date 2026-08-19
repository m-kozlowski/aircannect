#include "storage_scan_port.h"

#include "memory_manager.h"

namespace aircannect {

StorageScanSnapshot::~StorageScanSnapshot() {
    Memory::free(entries_);
    Memory::free(paths_);
}

bool StorageScanSnapshot::entry(size_t index,
                                StorageScanEntryView &out) const {
    out = StorageScanEntryView();
    if (index >= entry_count_ || !entries_ || !paths_) return false;

    const Entry &entry = entries_[index];
    if (entry.path_offset >= paths_length_) return false;

    out.path = paths_ + entry.path_offset;
    out.directory = entry.directory;
    out.root_index = entry.root_index;
    out.size = entry.size;
    out.modified = entry.modified;
    return true;
}

}  // namespace aircannect
