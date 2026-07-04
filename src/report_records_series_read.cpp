#include "report_records.h"

#include "report_records_internal.h"

#include <limits.h>

namespace aircannect {
namespace {

using report_records_detail::bitmap_missing;
using report_records_detail::get_le32;
using report_records_detail::parse_series_v2_header;
using report_records_detail::SERIES_V2_MODE_EXPLICIT;
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
    if (!callback || record_count == 0 ||
        payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2 ||
        !valid_timestamp(chunk_start_ms)) {
        return false;
    }

    SeriesV2HeaderView header;
    if (!parse_series_v2_header(data, len, record_count, header)) {
        return false;
    }

    if (header.mode == SERIES_V2_MODE_UNIFORM) {
        if (header.interval_ms == 0 ||
            static_cast<size_t>(header.sample_count) > SIZE_MAX / 4u ||
            header.body_len !=
                static_cast<size_t>(header.sample_count) * 4u) {
            return false;
        }
        if (static_cast<int64_t>(header.interval_ms) >
            INT64_MAX - chunk_start_ms) {
            return false;
        }

        const int64_t max_steps =
            (INT64_MAX - chunk_start_ms) /
            static_cast<int64_t>(header.interval_ms);
        if (static_cast<int64_t>(header.sample_count - 1) > max_steps) {
            return false;
        }

        for (uint32_t i = 0; i < header.sample_count; ++i) {
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

    if (header.mode == SERIES_V2_MODE_EXPLICIT) {
        if (header.interval_ms != 0 ||
            header.missing_bitmap_bytes != 0 ||
            static_cast<size_t>(header.sample_count) > SIZE_MAX / 8u ||
            header.body_len !=
                static_cast<size_t>(header.sample_count) * 8u) {
            return false;
        }

        for (uint32_t i = 0; i < header.sample_count; ++i) {
            const uint8_t *record = header.body + i * 8u;
            const int32_t offset_ms =
                static_cast<int32_t>(get_le32(record + 0));
            const int64_t timestamp_ms =
                chunk_start_ms + static_cast<int64_t>(offset_ms);
            if (!valid_timestamp(timestamp_ms)) return false;

            ReportSeriesSample sample;
            sample.timestamp_ms = timestamp_ms;
            sample.value_milli =
                static_cast<int32_t>(get_le32(record + 4));

            if (!callback(context, sample)) return false;
        }

        return true;
    }

    return false;
}

}  // namespace aircannect
