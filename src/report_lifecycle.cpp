#include "report_manager.h"

namespace aircannect {

ReportManager::ReportManager(RpcRequestPort &rpc)
    : fetch_runtime_(rpc),
      cache_write_sink_(cache_storage_, summary_),
      night_index_service_(summary_, night_index_, edf_catalog_),
      night_query_(night_index_service_),
      summary_service_(summary_,
                       fetch_runtime_,
                       night_index_,
                       night_index_service_,
                       result_cache_),
      cache_fetch_(fetch_runtime_,
                   summary_,
                   cache_storage_,
                   cache_write_sink_,
                   edf_catalog_),
      cache_write_service_(cache_storage_,
                           fetch_runtime_,
                           summary_,
                           night_index_,
                           result_cache_),
      range_plot_builder_(range_plot_runtime_,
                          result_cache_,
                          night_index_service_,
                          edf_catalog_),
      result_build_(result_cache_, edf_catalog_),
      result_prepare_(result_build_,
                      result_cache_,
                      night_index_service_,
                      cache_fetch_),
      night_cache_(night_index_service_,
                   edf_catalog_,
                   cache_fetch_),
      prefetch_service_(prefetch_,
                        cache_fetch_,
                        night_cache_,
                        summary_service_,
                        result_build_,
                        range_plot_builder_,
                        edf_catalog_),
      plot_prebuild_(summary_service_,
                     cache_fetch_,
                     result_build_,
                     range_plot_builder_,
                     result_cache_,
                     night_index_service_,
                     build_runtime_),
      build_queue_service_(build_runtime_,
                           night_index_service_,
                           result_cache_),
      cache_maintenance_(summary_,
                         night_index_,
                         night_index_service_,
                         summary_service_,
                         cache_fetch_,
                         result_cache_,
                         result_build_,
                         range_plot_builder_,
                         result_prepare_,
                         build_queue_service_),
      result_serving_(night_index_service_,
                      build_runtime_,
                      result_cache_,
                      result_build_.runtime()),
      runtime_service_(fetch_runtime_,
                       summary_,
                       cache_storage_,
                       result_cache_,
                       summary_service_,
                       cache_fetch_,
                       result_build_,
                       range_plot_builder_,
                       result_prepare_,
                       plot_prebuild_,
                       build_queue_service_,
                       prefetch_service_,
                       edf_catalog_) {}

ReportManager::~ReportManager() {
    cache_storage_.release();
}

void ReportManager::begin() {
    summary_service_.begin();
    night_index_.begin();
    night_index_service_.begin();
    result_cache_.begin();
    prefetch_.begin();
    build_runtime_.begin();
    cache_storage_.begin();
    night_index_.load_durable();

    summary_service_.load_initial_snapshot();
}

void ReportManager::set_edf_report_catalog(EdfReportCatalogJob *catalog) {
    edf_catalog_.set_catalog(catalog);
}

}  // namespace aircannect
