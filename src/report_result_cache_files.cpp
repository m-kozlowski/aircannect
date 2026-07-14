#include "report_result_cache_files.h"

#include "report_cache_paths.h"
#include "storage_manager.h"

namespace aircannect {

bool result_plot_cache_exists_for_etag(uint64_t night_start_ms,
                                       const char *etag) {
    Storage::Guard g;
    if (!Storage::mounted()) return false;

    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_etag(night_start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }

    return Storage::exists(path);
}

}  // namespace aircannect
