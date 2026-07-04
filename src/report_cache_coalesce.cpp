#include "report_manager.h"

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

// Coalesce parsed chunks per compatible (kind,name) segment. A night/timebase
// break or size cap opens another active segment instead of forcing an immediate
// writer-queue flush from the parser callback; source finalization drains the
// pending segments with normal backpressure handling.
bool ReportManager::ensure_cache_coalesce_slots() {
    if (cache_coalesce_) return true;

    cache_coalesce_ = static_cast<CacheCoalesceBuffer *>(Memory::alloc_large(
        AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer), false));
    if (!cache_coalesce_) {
        log_report_alloc_failed(
            "cache_coalesce",
            AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        new (&cache_coalesce_[i]) CacheCoalesceBuffer();
    }
    return true;
}

bool ReportManager::buffer_parsed_chunk(const ReportParsedChunk &chunk) {
    if (!ensure_cache_coalesce_slots()) {
        fail_cache_fetch("cache_coalesce_alloc_failed");
        return false;
    }

    const int64_t night = night_start_for_timestamp(chunk.start_ms);
    const bool is_series = chunk.kind == ReportStoreChunkKind::Series;
    ReportSeriesV2UniformView series_view;
    if (is_series) {
        if (!report_series_payload_v2_uniform_view(chunk.payload,
                                                   chunk.payload_len,
                                                   chunk.record_count,
                                                   series_view) ||
            series_view.missing_bitmap_bytes != 0) {
            fail_cache_fetch("cache_series_v2_invalid");
            return false;
        }
    }

    const size_t incoming_bytes =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    const size_t fixed_bytes =
        is_series ? report_series_v2_header_size() : 0;

    size_t slot = AC_REPORT_COALESCE_SLOTS;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        if (cache_coalesce_[i].active &&
            cache_coalesce_[i].kind == chunk.kind &&
            cache_coalesce_[i].name == chunk.name) {
            CacheCoalesceBuffer &buf = cache_coalesce_[i];
            bool size_overflow = incoming_bytes > SIZE_MAX - buf.payload.size();
            size_t projected_size =
                size_overflow ? SIZE_MAX : buf.payload.size() + incoming_bytes;
            if (!size_overflow) {
                size_overflow = fixed_bytes > SIZE_MAX - projected_size;
                if (!size_overflow) projected_size += fixed_bytes;
            }
            if (size_overflow ||
                projected_size > AC_REPORT_COALESCE_TARGET_BYTES) {
                continue;
            }
            if (buf.night_start_ms != night) continue;
            if (is_series &&
                (buf.series_interval_ms != series_view.interval_ms ||
                 chunk.start_ms != buf.last_ms)) {
                continue;
            }
            slot = i;
            break;
        }
    }

    if (slot == AC_REPORT_COALESCE_SLOTS) {
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            if (!cache_coalesce_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == AC_REPORT_COALESCE_SLOTS) {
            const CacheFlushResult flush = flush_cache_coalesce_buffer(0);
            if (flush == CacheFlushResult::Blocked) {
                cache_status_.error = "cache_coalesce_backpressure";
                return false;
            }
            if (flush == CacheFlushResult::Failed) {
                cache_status_.error = "cache_coalesce_flush_failed";
                return false;
            }
            slot = 0;
        }

        CacheCoalesceBuffer &buf = cache_coalesce_[slot];
        buf.active = true;
        buf.kind = chunk.kind;
        buf.source = chunk.source;
        buf.name = chunk.name;
        buf.night_start_ms = night;
        buf.first_ms = chunk.start_ms;
        buf.last_ms = chunk.end_ms;
        buf.record_count = 0;
        buf.payload_schema = chunk.payload_schema;
        buf.series_interval_ms = is_series ? series_view.interval_ms : 0;
        buf.series_values_pending = is_series;
        buf.payload.clear();
        buf.payload.set_max_size(AC_REPORT_COALESCE_TARGET_BYTES + 65536);
    }

    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    const uint8_t *append_data =
        is_series ? series_view.values_milli_le : chunk.payload;
    const size_t append_len =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    if (!buf.payload.append(append_data, append_len)) {
        fail_cache_fetch("cache_coalesce_alloc");
        return false;
    }

    if (chunk.start_ms < buf.first_ms) buf.first_ms = chunk.start_ms;
    if (chunk.end_ms > buf.last_ms) buf.last_ms = chunk.end_ms;
    buf.record_count += chunk.record_count;
    note_cache_chunk_coverage(chunk);
    return true;
}

report_manager_internal::CacheFlushResult
ReportManager::flush_cache_coalesce_buffer(
    size_t slot) {
    if (!cache_coalesce_ || slot >= AC_REPORT_COALESCE_SLOTS) {
        return CacheFlushResult::Flushed;
    }

    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    if (!buf.active) return CacheFlushResult::Flushed;

    CacheFlushResult result = CacheFlushResult::Flushed;
    if (buf.record_count > 0 && buf.payload.size() > 0 &&
        buf.last_ms > buf.first_ms) {
        ReportStoreChunkKey key;
        key.kind = buf.kind;
        key.source = report_source_spool_type(buf.source);
        key.name = buf.name;
        key.start_ms = buf.first_ms;
        key.end_ms = buf.last_ms;
        key.night_start_ms = buf.night_start_ms;
        if (!key.source || !key.source[0]) {
            result = CacheFlushResult::Failed;
        } else {
            ReportStoreChunkMeta meta;
            meta.record_count = buf.record_count;
            meta.payload_schema = buf.payload_schema;
            if (buf.kind == ReportStoreChunkKind::Series &&
                buf.series_values_pending) {
                ReportSpoolBuffer built;
                if (!report_build_series_payload_v2_uniform_values_le(
                        built,
                        buf.series_interval_ms,
                        buf.payload.data(),
                        buf.record_count)) {
                    Log::logf(CAT_REPORT,
                              LOG_WARN,
                              "Cache series v2 build failed source=%s "
                              "name=%s records=%lu\n",
                              key.source ? key.source : "",
                              key.name ? key.name : "",
                              static_cast<unsigned long>(buf.record_count));
                    result = CacheFlushResult::Failed;
                } else {
                    buf.payload.move_from(built);
                    buf.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
                    buf.series_values_pending = false;
                    meta.payload_schema = buf.payload_schema;
                }
            }
            if (result == CacheFlushResult::Flushed) {
                const CacheWriteEnqueueResult queued =
                    enqueue_cache_write(buf, key, meta);
                if (queued == CacheWriteEnqueueResult::Blocked) {
                    return CacheFlushResult::Blocked;
                }
                if (queued == CacheWriteEnqueueResult::Failed) {
                    result = CacheFlushResult::Failed;
                }
            }
        }
    }

    buf.active = false;
    buf.payload.clear();
    buf.record_count = 0;
    buf.first_ms = 0;
    buf.last_ms = 0;
    buf.night_start_ms = 0;
    buf.name = nullptr;
    buf.payload_schema = 0;
    buf.series_interval_ms = 0;
    buf.series_values_pending = false;
    return result;
}

report_manager_internal::CacheFlushResult
ReportManager::flush_all_cache_coalesce_buffers() {
    if (!cache_coalesce_) return CacheFlushResult::Flushed;

    CacheFlushResult result = CacheFlushResult::Flushed;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        const CacheFlushResult flush = flush_cache_coalesce_buffer(i);
        if (flush == CacheFlushResult::Failed) return flush;
        if (flush == CacheFlushResult::Blocked) {
            result = CacheFlushResult::Blocked;
        }
    }
    return result;
}

void ReportManager::discard_cache_coalesce_buffers() {
    if (!cache_coalesce_) return;

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        CacheCoalesceBuffer &buf = cache_coalesce_[i];
        buf.active = false;
        buf.payload.clear();
        buf.record_count = 0;
        buf.first_ms = 0;
        buf.last_ms = 0;
        buf.night_start_ms = 0;
        buf.name = nullptr;
        buf.payload_schema = 0;
        buf.series_interval_ms = 0;
        buf.series_values_pending = false;
    }
}

}  // namespace aircannect
