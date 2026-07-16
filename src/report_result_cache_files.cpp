#include "report_result_cache_files.h"

#include "report_cache_paths.h"
#include "storage_manager.h"

namespace aircannect {

bool result_cache_pair_exists_for_etag(uint64_t night_start_ms,
                                       const char *etag) {
    Storage::Guard g;
    if (!Storage::mounted()) return false;

    char plot_path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_etag(night_start_ms,
                                         etag,
                                         plot_path,
                                         sizeof(plot_path))) {
        return false;
    }

    char result_path[REPORT_CACHE_PATH_MAX];
    if (!result_json_cache_path_for_etag(night_start_ms,
                                         etag,
                                         result_path,
                                         sizeof(result_path))) {
        return false;
    }

    return Storage::exists(result_path) && Storage::exists(plot_path);
}

}  // namespace aircannect
