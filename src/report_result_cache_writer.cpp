#include "report_manager.h"

#include <algorithm>
#include <stdio.h>

#include "background_worker.h"
#include "debug_log.h"
#include "report_cache_paths.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

constexpr size_t REPORT_CACHE_WRITE_CHUNK = 4096;

}  // namespace

void ReportManager::reset_result_cache_write_locked() {
    if (plot_cache_write_.file) {
        Storage::Guard g;
        plot_cache_write_.file.close();
    }

    plot_cache_write_.file = File();
    plot_cache_write_.active = false;
    plot_cache_write_.phase = ResultCacheWritePhase::Idle;
    plot_cache_write_.night = ReportSummaryRecord{};
    plot_cache_write_.plot_path[0] = 0;
    plot_cache_write_.plot_tmp_path[0] = 0;
    plot_cache_write_.result_path[0] = 0;
    plot_cache_write_.result_tmp_path[0] = 0;
    plot_cache_write_.result_json.reset();
    plot_cache_write_.plot.reset();
    plot_cache_write_.offset = 0;
}

bool ReportManager::enqueue_result_cache_write(
    const ReportIndexedNight &night,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &result_json,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!result_json || result_json->size() == 0 ||
        !plot || plot->size() == 0 || !plot_cache_write_lock_) {
        return false;
    }

    char path[sizeof(plot_cache_write_.plot_path)];
    if (!result_plot_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }

    char tmp[sizeof(plot_cache_write_.plot_tmp_path)];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(tmp)) {
        return false;
    }

    char result_path[sizeof(plot_cache_write_.result_path)];
    if (!result_json_cache_path_for_night(night,
                                          etag,
                                          result_path,
                                          sizeof(result_path))) {
        return false;
    }

    char result_tmp[sizeof(plot_cache_write_.result_tmp_path)];
    const int result_written =
        snprintf(result_tmp, sizeof(result_tmp), "%s.tmp", result_path);
    if (result_written <= 0 ||
        static_cast<size_t>(result_written) >= sizeof(result_tmp)) {
        return false;
    }

    if (xSemaphoreTake(plot_cache_write_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    if (plot_cache_write_.active) {
        xSemaphoreGive(plot_cache_write_lock_);
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Plot cache write skipped: writer busy night=%llu\n",
                  static_cast<unsigned long long>(night.summary.start_ms));
        return false;
    }

    reset_result_cache_write_locked();
    plot_cache_write_.active = true;
    plot_cache_write_.phase = ResultCacheWritePhase::ClearOld;
    plot_cache_write_.night = night.summary;
    snprintf(plot_cache_write_.plot_path,
             sizeof(plot_cache_write_.plot_path),
             "%s",
             path);
    snprintf(plot_cache_write_.plot_tmp_path,
             sizeof(plot_cache_write_.plot_tmp_path),
             "%s",
             tmp);
    snprintf(plot_cache_write_.result_path,
             sizeof(plot_cache_write_.result_path),
             "%s",
             result_path);
    snprintf(plot_cache_write_.result_tmp_path,
             sizeof(plot_cache_write_.result_tmp_path),
             "%s",
             result_tmp);
    plot_cache_write_.result_json = result_json;
    plot_cache_write_.plot = plot;
    plot_cache_write_.offset = 0;
    xSemaphoreGive(plot_cache_write_lock_);

    if (BackgroundWorker *worker = background_worker()) {
        worker->wake();
    }

    return true;
}

bool ReportManager::plot_cache_writer_active() const {
    if (!plot_cache_write_lock_) return false;

    if (xSemaphoreTake(plot_cache_write_lock_, pdMS_TO_TICKS(1)) != pdTRUE) {
        return true;
    }

    const bool active = plot_cache_write_.active;
    xSemaphoreGive(plot_cache_write_lock_);
    return active;
}

bool ReportManager::service_result_cache_writer() {
    if (!plot_cache_write_lock_) return false;

    if (xSemaphoreTake(plot_cache_write_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    if (!plot_cache_write_.active) {
        xSemaphoreGive(plot_cache_write_lock_);
        return false;
    }

    bool ok = true;
    switch (plot_cache_write_.phase) {
        case ResultCacheWritePhase::ClearOld: {
            {
                Storage::Guard g;
                ok = Storage::mounted() &&
                     Storage::ensure_dir("/aircannect") &&
                     Storage::ensure_dir("/aircannect/report") &&
                     Storage::ensure_dir(REPORT_CACHE_BASE_DIR) &&
                     Storage::ensure_dir("/aircannect/report/v4/plots") &&
                     Storage::ensure_dir(REPORT_PLOT_CACHE_DIR) &&
                     Storage::ensure_dir("/aircannect/report/v4/results") &&
                     Storage::ensure_dir(REPORT_RESULT_JSON_CACHE_DIR);
            }

            uint32_t deleted_plot = 0;
            uint32_t deleted_result = 0;
            ok = ok && clear_plot_cache_for_night(plot_cache_write_.night,
                                                  deleted_plot);
            ok = ok && clear_result_json_cache_for_night(
                           plot_cache_write_.night,
                           deleted_result);
            plot_cache_write_.phase =
                ok ? ResultCacheWritePhase::OpenPlotTmp
                   : ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::OpenPlotTmp: {
            Storage::Guard g;
            Storage::remove(plot_cache_write_.plot_tmp_path);
            plot_cache_write_.file =
                Storage::open(plot_cache_write_.plot_tmp_path, "w");
            ok = static_cast<bool>(plot_cache_write_.file);
            plot_cache_write_.phase =
                ok ? ResultCacheWritePhase::WritePlot
                   : ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::WritePlot: {
            if (!plot_cache_write_.plot || !plot_cache_write_.file) {
                ok = false;
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
                break;
            }

            const size_t total = plot_cache_write_.plot->size();
            if (plot_cache_write_.offset >= total) {
                plot_cache_write_.phase =
                    ResultCacheWritePhase::ClosePlotRename;
                break;
            }

            const size_t len =
                std::min(REPORT_CACHE_WRITE_CHUNK,
                         total - plot_cache_write_.offset);
            const uint8_t *data =
                plot_cache_write_.plot->data() + plot_cache_write_.offset;

            size_t wrote = 0;
            {
                Storage::Guard g;
                wrote = plot_cache_write_.file.write(data, len);
            }

            ok = wrote == len;
            if (ok) {
                plot_cache_write_.offset += len;
                if (plot_cache_write_.offset >= total) {
                    plot_cache_write_.phase =
                        ResultCacheWritePhase::ClosePlotRename;
                }
            } else {
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            }
            break;
        }

        case ResultCacheWritePhase::ClosePlotRename: {
            Storage::Guard g;
            if (plot_cache_write_.file) plot_cache_write_.file.close();
            Storage::remove(plot_cache_write_.plot_path);
            ok = Storage::rename(plot_cache_write_.plot_tmp_path,
                                 plot_cache_write_.plot_path);
            if (!ok) {
                Storage::remove(plot_cache_write_.plot_tmp_path);
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            } else {
                plot_cache_write_.offset = 0;
                plot_cache_write_.phase =
                    ResultCacheWritePhase::OpenResultTmp;
            }
            break;
        }

        case ResultCacheWritePhase::OpenResultTmp: {
            Storage::Guard g;
            Storage::remove(plot_cache_write_.result_tmp_path);
            plot_cache_write_.file =
                Storage::open(plot_cache_write_.result_tmp_path, "w");
            ok = static_cast<bool>(plot_cache_write_.file);
            plot_cache_write_.phase =
                ok ? ResultCacheWritePhase::WriteResult
                   : ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::WriteResult: {
            if (!plot_cache_write_.result_json || !plot_cache_write_.file) {
                ok = false;
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
                break;
            }

            const size_t total = plot_cache_write_.result_json->size();
            if (plot_cache_write_.offset >= total) {
                plot_cache_write_.phase =
                    ResultCacheWritePhase::CloseResultRename;
                break;
            }

            const size_t len =
                std::min(REPORT_CACHE_WRITE_CHUNK,
                         total - plot_cache_write_.offset);
            const uint8_t *data =
                plot_cache_write_.result_json->data() +
                plot_cache_write_.offset;

            size_t wrote = 0;
            {
                Storage::Guard g;
                wrote = plot_cache_write_.file.write(data, len);
            }

            ok = wrote == len;
            if (ok) {
                plot_cache_write_.offset += len;
                if (plot_cache_write_.offset >= total) {
                    plot_cache_write_.phase =
                        ResultCacheWritePhase::CloseResultRename;
                }
            } else {
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            }
            break;
        }

        case ResultCacheWritePhase::CloseResultRename: {
            Storage::Guard g;
            if (plot_cache_write_.file) plot_cache_write_.file.close();
            Storage::remove(plot_cache_write_.result_path);
            ok = Storage::rename(plot_cache_write_.result_tmp_path,
                                 plot_cache_write_.result_path);
            if (!ok) Storage::remove(plot_cache_write_.result_tmp_path);
            plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::Idle:
        default:
            ok = false;
            break;
    }

    const bool done = plot_cache_write_.phase == ResultCacheWritePhase::Idle;
    const uint64_t night_start_ms = plot_cache_write_.night.start_ms;
    const size_t plot_bytes =
        plot_cache_write_.plot ? plot_cache_write_.plot->size() : 0;
    const size_t result_bytes =
        plot_cache_write_.result_json ? plot_cache_write_.result_json->size()
                                      : 0;
    if (done) {
        if (ok) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result cache write complete night=%llu result=%u "
                      "plot=%u\n",
                      static_cast<unsigned long long>(night_start_ms),
                      static_cast<unsigned>(result_bytes),
                      static_cast<unsigned>(plot_bytes));
        } else {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "Result cache write failed night=%llu result=%u "
                      "plot=%u\n",
                      static_cast<unsigned long long>(night_start_ms),
                      static_cast<unsigned>(result_bytes),
                      static_cast<unsigned>(plot_bytes));
            Storage::Guard g;
            if (plot_cache_write_.file) plot_cache_write_.file.close();
            if (plot_cache_write_.plot_tmp_path[0]) {
                Storage::remove(plot_cache_write_.plot_tmp_path);
            }
            if (plot_cache_write_.result_tmp_path[0]) {
                Storage::remove(plot_cache_write_.result_tmp_path);
            }
        }

        reset_result_cache_write_locked();
    }

    xSemaphoreGive(plot_cache_write_lock_);
    return true;
}

}  // namespace aircannect
