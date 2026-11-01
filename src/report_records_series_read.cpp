#include "report_records.h"

#include "report_records_internal.h"

#include <limits.h>

namespace aircannect {
namespace {

using report_records_detail::bitmap_missing;
using report_records_detail::get_le32;
using report_records_detail::valid_timestamp;

}  // namespace

bool report_series_payload_v2_uniform_view(
    const uint8_t *data,
    size_t len,
    uint32_t record_count,
    ReportSeriesV2UniformView &view) {
    view = {};

    ReportSeriesV2UniformLayout header;
    if (!report_series_payload_v2_uniform_layout(
            data, len, len, record_count, header)) {
        return false;
    }

    view.interval_ms = header.interval_ms;
    view.sample_count = header.sample_count;
    view.missing_bitmap = header.missing_bitmap_bytes
        ? data + ReportSeriesV2UniformLayout::HeaderBytes : nullptr;
    view.missing_bitmap_bytes = header.missing_bitmap_bytes;
    view.values_milli_le = data + header.values_offset();
    view.values_milli_bytes = len - header.values_offset();
    return true;
}

bool report_for_each_series_sample(uint32_t payload_schema,
                                   int64_t chunk_start_ms,
                                   const uint8_t *data,
                                   size_t len,
                                   uint32_t record_count,
                                   ReportSeriesSampleCallback callback,
                                   void *context) {
    return report_for_each_series_sample_range(payload_schema,
                                               chunk_start_ms,
                                               data,
                                               len,
                                               record_count,
                                               0,
                                               record_count,
                                               callback,
                                               context);
}

bool report_for_each_series_sample_range(
    uint32_t payload_schema,
    int64_t chunk_start_ms,
    const uint8_t *data,
    size_t len,
    uint32_t record_count,
    uint32_t first_sample,
    uint32_t sample_count,
    ReportSeriesSampleCallback callback,
    void *context) {
    if (!callback || record_count == 0 ||
        payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2 ||
        !valid_timestamp(chunk_start_ms)) {
        return false;
    }

    ReportSeriesV2UniformView header;
    if (!report_series_payload_v2_uniform_view(data, len, record_count, header)) {
        return false;
    }

    if (sample_count == 0 || first_sample > header.sample_count ||
        sample_count > header.sample_count - first_sample) {
        return false;
    }

    return report_for_each_series_v2_uniform_slice(
        chunk_start_ms, header.interval_ms, first_sample,
        header.values_milli_le + static_cast<size_t>(first_sample) * 4u,
        static_cast<size_t>(sample_count) * 4u, sample_count, callback, context,
        header.missing_bitmap, header.missing_bitmap_bytes);
}

bool report_for_each_series_v2_uniform_slice(
    int64_t chunk_start_ms,
    uint32_t interval_ms,
    uint32_t first_sample,
    const uint8_t *values_milli_le,
    size_t values_milli_bytes,
    uint32_t sample_count,
    ReportSeriesSampleCallback callback,
    void *context,
    const uint8_t *missing_bitmap,
    size_t missing_bitmap_bytes) {
    if (!callback || !values_milli_le || !valid_timestamp(chunk_start_ms) ||
        interval_ms == 0 || sample_count == 0 ||
        (missing_bitmap_bytes && !missing_bitmap) ||
        first_sample > UINT32_MAX - (sample_count - 1u) ||
        static_cast<size_t>(sample_count) > SIZE_MAX / sizeof(int32_t) ||
        values_milli_bytes !=
            static_cast<size_t>(sample_count) * sizeof(int32_t)) {
        return false;
    }

    const uint64_t last_sample =
        static_cast<uint64_t>(first_sample) + sample_count - 1u;
    if (last_sample > static_cast<uint64_t>(INT64_MAX - chunk_start_ms) /
            interval_ms) {
        return false;
    }

    for (uint32_t i = 0; i < sample_count; ++i) {
        const uint32_t sample_index = first_sample + i;
        if (bitmap_missing(missing_bitmap, missing_bitmap_bytes, sample_index)) {
            continue;
        }

        ReportSeriesSample sample;
        sample.timestamp_ms = chunk_start_ms +
            static_cast<int64_t>(sample_index) * interval_ms;
        sample.value_milli = static_cast<int32_t>(
            get_le32(values_milli_le + static_cast<size_t>(i) * 4u));

        if (!callback(context, sample)) return false;
    }

    return true;
}

}  // namespace aircannect
