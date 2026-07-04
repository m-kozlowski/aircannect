#include "report_result_cache_writer.h"

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

ReportResultCacheWriter::~ReportResultCacheWriter() {
    if (lock_) {
        if (xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE) {
            reset_locked();
            xSemaphoreGive(lock_);
        }

        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

bool ReportResultCacheWriter::begin() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool ReportResultCacheWriter::enqueue(
    const ReportIndexedNight &night,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &result_json,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!result_json || result_json->size() == 0 ||
        !plot || plot->size() == 0 || !lock_) {
        return false;
    }

    char path[sizeof(job_.plot_path)];
    if (!result_plot_cache_path_for_etag(night.summary.start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }

    char tmp[sizeof(job_.plot_tmp_path)];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(tmp)) {
        return false;
    }

    char result_path[sizeof(job_.result_path)];
    if (!result_json_cache_path_for_etag(night.summary.start_ms,
                                         etag,
                                         result_path,
                                         sizeof(result_path))) {
        return false;
    }

    char result_tmp[sizeof(job_.result_tmp_path)];
    const int result_written =
        snprintf(result_tmp, sizeof(result_tmp), "%s.tmp", result_path);
    if (result_written <= 0 ||
        static_cast<size_t>(result_written) >= sizeof(result_tmp)) {
        return false;
    }

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return false;

    if (job_.active) {
        xSemaphoreGive(lock_);
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Plot cache write skipped: writer busy night=%llu\n",
                  static_cast<unsigned long long>(night.summary.start_ms));
        return false;
    }

    reset_locked();
    job_.active = true;
    job_.phase = ResultCacheWritePhase::ClearOld;
    job_.night = night.summary;
    snprintf(job_.plot_path, sizeof(job_.plot_path), "%s", path);
    snprintf(job_.plot_tmp_path, sizeof(job_.plot_tmp_path), "%s", tmp);
    snprintf(job_.result_path, sizeof(job_.result_path), "%s", result_path);
    snprintf(job_.result_tmp_path,
             sizeof(job_.result_tmp_path),
             "%s",
             result_tmp);
    job_.result_json = result_json;
    job_.plot = plot;
    job_.offset = 0;
    xSemaphoreGive(lock_);

    if (BackgroundWorker *worker = background_worker()) {
        worker->wake();
    }

    return true;
}

bool ReportResultCacheWriter::active() const {
    if (!lock_) return false;

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(1)) != pdTRUE) return true;

    const bool writer_active = job_.active;
    xSemaphoreGive(lock_);
    return writer_active;
}

bool ReportResultCacheWriter::service() {
    if (!lock_) return false;

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    if (!job_.active) {
        xSemaphoreGive(lock_);
        return false;
    }

    bool ok = true;
    switch (job_.phase) {
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
            ok = ok && clear_result_plot_cache_for_night(job_.night.start_ms,
                                                         deleted_plot);
            ok = ok && clear_result_json_cache_for_night(job_.night.start_ms,
                                                         deleted_result);
            job_.phase =
                ok ? ResultCacheWritePhase::OpenPlotTmp
                   : ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::OpenPlotTmp: {
            Storage::Guard g;
            Storage::remove(job_.plot_tmp_path);
            job_.file = Storage::open(job_.plot_tmp_path, "w");
            ok = static_cast<bool>(job_.file);
            job_.phase =
                ok ? ResultCacheWritePhase::WritePlot
                   : ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::WritePlot: {
            if (!job_.plot || !job_.file) {
                ok = false;
                job_.phase = ResultCacheWritePhase::Idle;
                break;
            }

            const size_t total = job_.plot->size();
            if (job_.offset >= total) {
                job_.phase = ResultCacheWritePhase::ClosePlotRename;
                break;
            }

            const size_t len =
                std::min(REPORT_CACHE_WRITE_CHUNK, total - job_.offset);
            const uint8_t *data = job_.plot->data() + job_.offset;

            size_t wrote = 0;
            {
                Storage::Guard g;
                wrote = job_.file.write(data, len);
            }

            ok = wrote == len;
            if (ok) {
                job_.offset += len;
                if (job_.offset >= total) {
                    job_.phase = ResultCacheWritePhase::ClosePlotRename;
                }
            } else {
                job_.phase = ResultCacheWritePhase::Idle;
            }
            break;
        }

        case ResultCacheWritePhase::ClosePlotRename: {
            Storage::Guard g;
            if (job_.file) job_.file.close();
            Storage::remove(job_.plot_path);
            ok = Storage::rename(job_.plot_tmp_path, job_.plot_path);
            if (!ok) {
                Storage::remove(job_.plot_tmp_path);
                job_.phase = ResultCacheWritePhase::Idle;
            } else {
                job_.offset = 0;
                job_.phase = ResultCacheWritePhase::OpenResultTmp;
            }
            break;
        }

        case ResultCacheWritePhase::OpenResultTmp: {
            Storage::Guard g;
            Storage::remove(job_.result_tmp_path);
            job_.file = Storage::open(job_.result_tmp_path, "w");
            ok = static_cast<bool>(job_.file);
            job_.phase =
                ok ? ResultCacheWritePhase::WriteResult
                   : ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::WriteResult: {
            if (!job_.result_json || !job_.file) {
                ok = false;
                job_.phase = ResultCacheWritePhase::Idle;
                break;
            }

            const size_t total = job_.result_json->size();
            if (job_.offset >= total) {
                job_.phase = ResultCacheWritePhase::CloseResultRename;
                break;
            }

            const size_t len =
                std::min(REPORT_CACHE_WRITE_CHUNK, total - job_.offset);
            const uint8_t *data = job_.result_json->data() + job_.offset;

            size_t wrote = 0;
            {
                Storage::Guard g;
                wrote = job_.file.write(data, len);
            }

            ok = wrote == len;
            if (ok) {
                job_.offset += len;
                if (job_.offset >= total) {
                    job_.phase = ResultCacheWritePhase::CloseResultRename;
                }
            } else {
                job_.phase = ResultCacheWritePhase::Idle;
            }
            break;
        }

        case ResultCacheWritePhase::CloseResultRename: {
            Storage::Guard g;
            if (job_.file) job_.file.close();
            Storage::remove(job_.result_path);
            ok = Storage::rename(job_.result_tmp_path, job_.result_path);
            if (!ok) Storage::remove(job_.result_tmp_path);
            job_.phase = ResultCacheWritePhase::Idle;
            break;
        }

        case ResultCacheWritePhase::Idle:
        default:
            ok = false;
            break;
    }

    const bool done =
        job_.phase == ResultCacheWritePhase::Idle;
    const uint64_t night_start_ms = job_.night.start_ms;
    const size_t plot_bytes = job_.plot ? job_.plot->size() : 0;
    const size_t result_bytes =
        job_.result_json ? job_.result_json->size() : 0;
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
            if (job_.file) job_.file.close();
            if (job_.plot_tmp_path[0]) Storage::remove(job_.plot_tmp_path);
            if (job_.result_tmp_path[0]) Storage::remove(job_.result_tmp_path);
        }

        reset_locked();
    }

    xSemaphoreGive(lock_);
    return true;
}

void ReportResultCacheWriter::reset_locked() {
    if (job_.file) {
        Storage::Guard g;
        job_.file.close();
    }

    job_.file = File();
    job_.active = false;
    job_.phase = ResultCacheWritePhase::Idle;
    job_.night = ReportSummaryRecord{};
    job_.plot_path[0] = 0;
    job_.plot_tmp_path[0] = 0;
    job_.result_path[0] = 0;
    job_.result_tmp_path[0] = 0;
    job_.result_json.reset();
    job_.plot.reset();
    job_.offset = 0;
}

}  // namespace aircannect
