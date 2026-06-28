#include "report_data_provider.h"

#include <stdint.h>

namespace aircannect {

bool ReportDataProvider::for_each_series_sample(
    const ReportProviderChunk &chunk,
    int64_t night_start_ms,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) const {
    stats = {};
    if (!callback || chunk.kind != ReportStoreChunkKind::Series) {
        return false;
    }

    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!read_chunk(chunk, night_start_ms, meta, payload)) {
        return false;
    }

    stats.record_count = meta.record_count;
    stats.payload_bytes = static_cast<uint32_t>(
        payload.size() > UINT32_MAX ? UINT32_MAX : payload.size());
    ReportSeriesV2UniformView view;
    if (report_series_payload_v2_uniform_view(payload.data(),
                                              payload.size(),
                                              meta.record_count,
                                              view)) {
        stats.interval_ms = view.interval_ms;
    }

    return report_for_each_series_sample(meta.payload_schema,
                                         chunk.start_ms,
                                         payload.data(),
                                         payload.size(),
                                         meta.record_count,
                                         callback,
                                         context);
}

}  // namespace aircannect
