#include "report_records.h"

#include "report_records_internal.h"

#include <limits.h>

namespace aircannect {
namespace {

using report_records_detail::bitmap_missing;
using report_records_detail::get_le32;
using report_records_detail::parse_series_v2_header;
using report_records_detail::SERIES_V2_MODE_UNIFORM;
using report_records_detail::SeriesV2HeaderView;
using report_records_detail::valid_timestamp;

}  // namespace

bool report_series_payload_v2_uniform_view(
    const uint8_t *data,
    size_t len,
    uint32_t record_count,
    ReportSeriesV2UniformView &view) {
    view = {};

    SeriesV2HeaderView header;
    if (!parse_series_v2_header(data, len, record_count, header)) {
        return false;
    }

    if (header.mode != SERIES_V2_MODE_UNIFORM ||
        header.interval_ms == 0 ||
        static_cast<size_t>(header.sample_count) > SIZE_MAX / 4u ||
        header.body_len != static_cast<size_t>(header.sample_count) * 4u) {
        return false;
    }

    view.interval_ms = header.interval_ms;
    view.sample_count = header.sample_count;
    view.missing_bitmap = header.missing_bitmap;
    view.missing_bitmap_bytes = header.missing_bitmap_bytes;
    view.values_milli_le = header.body;
    view.values_milli_bytes = header.body_len;
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

    SeriesV2HeaderView header;
    if (!parse_series_v2_header(data, len, record_count, header)) {
        return false;
    }

    if (header.mode != SERIES_V2_MODE_UNIFORM ||
        static_cast<size_t>(header.sample_count) > SIZE_MAX / 4u ||
        header.body_len != static_cast<size_t>(header.sample_count) * 4u ||
        sample_count == 0 || first_sample > header.sample_count ||
        sample_count > header.sample_count - first_sample) {
        return false;
    }
    if (static_cast<int64_t>(header.interval_ms) >
        INT64_MAX - chunk_start_ms) {
        return false;
    }

    const int64_t max_steps =
        (INT64_MAX - chunk_start_ms) /
        static_cast<int64_t>(header.interval_ms);
    const uint32_t last_sample = first_sample + sample_count - 1;
    if (static_cast<int64_t>(last_sample) > max_steps) {
        return false;
    }

    const uint32_t end_sample = first_sample + sample_count;
    for (uint32_t i = first_sample; i < end_sample; ++i) {
        if (bitmap_missing(header.missing_bitmap,
                           header.missing_bitmap_bytes,
                           i)) {
            continue;
        }

        ReportSeriesSample sample;
        sample.timestamp_ms =
            chunk_start_ms +
            static_cast<int64_t>(i) *
                static_cast<int64_t>(header.interval_ms);
        sample.value_milli =
            static_cast<int32_t>(get_le32(header.body + i * 4u));

        if (!callback(context, sample)) return false;
    }

    return true;
}

bool report_for_each_series_v2_uniform_unmasked_slice(
    int64_t chunk_start_ms,
    uint32_t interval_ms,
    uint32_t first_sample,
    const uint8_t *values_milli_le,
    size_t values_milli_bytes,
    uint32_t sample_count,
    ReportSeriesSampleCallback callback,
    void *context) {
    if (!callback || !values_milli_le || !valid_timestamp(chunk_start_ms) ||
        interval_ms == 0 || sample_count == 0 ||
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
