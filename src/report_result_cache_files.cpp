#include "report_result_cache_files.h"

#include "report_cache_paths.h"
#include "report_legacy_storage.h"

namespace aircannect {

bool result_cache_pair_exists_for_etag(uint64_t night_start_ms,
                                       const char *etag) {
    ReportLegacyStorageGuard g;
    if (!ReportLegacyStorage::mounted()) return false;

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

    return ReportLegacyStorage::exists(result_path) && ReportLegacyStorage::exists(plot_path);
}

}  // namespace aircannect
