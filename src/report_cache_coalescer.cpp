#include "report_cache_coalescer.h"

#include <new>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_manager_limits.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

namespace {

using CacheCoalesceBuffer = report_manager_internal::CacheCoalesceBuffer;
using CacheFlushResult = report_manager_internal::CacheFlushResult;
using CacheWriteEnqueueResult =
    report_manager_internal::CacheWriteEnqueueResult;

bool cache_coalesce_projected_size_fits(
    const CacheCoalesceBuffer &buf,
    size_t incoming_bytes,
    size_t fixed_bytes) {
    bool overflow = incoming_bytes > SIZE_MAX - buf.payload.size();
    size_t projected_size =
        overflow ? SIZE_MAX : buf.payload.size() + incoming_bytes;

    if (!overflow) {
        overflow = fixed_bytes > SIZE_MAX - projected_size;
        if (!overflow) projected_size += fixed_bytes;
    }

    return !overflow && projected_size <= AC_REPORT_COALESCE_TARGET_BYTES;
}

}  // namespace

const char *report_cache_coalesce_error(ReportCacheCoalesceResult result) {
    switch (result) {
        case ReportCacheCoalesceResult::Buffered:
            return "";
        case ReportCacheCoalesceResult::Backpressure:
            return "cache_coalesce_backpressure";
        case ReportCacheCoalesceResult::FlushFailed:
            return "cache_coalesce_flush_failed";
        case ReportCacheCoalesceResult::AllocFailed:
            return "cache_coalesce_alloc_failed";
        case ReportCacheCoalesceResult::InvalidSeries:
            return "cache_series_v2_invalid";
        case ReportCacheCoalesceResult::PayloadAllocFailed:
            return "cache_coalesce_alloc";
    }

    return "cache_coalesce_failed";
}

ReportCacheCoalescer::~ReportCacheCoalescer() {
    release();
}

bool ReportCacheCoalescer::begin() {
    if (slots_) return true;

    slots_ = static_cast<CacheCoalesceBuffer *>(Memory::alloc_large(
        AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer), false));
    if (!slots_) {
        log_report_alloc_failed(
            "cache_coalesce",
            AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        new (&slots_[i]) CacheCoalesceBuffer();
    }

    return true;
}

void ReportCacheCoalescer::release() {
    if (!slots_) return;

    discard();

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        slots_[i].~CacheCoalesceBuffer();
    }

    Memory::free(slots_);
    slots_ = nullptr;
}

ReportCacheCoalesceResult ReportCacheCoalescer::buffer(
    const ReportParsedChunk &chunk,
    int64_t night_start_ms,
    ReportCacheCoalescerSink &sink) {
    if (!begin()) return ReportCacheCoalesceResult::AllocFailed;

    const bool is_series = chunk.kind == ReportStoreChunkKind::Series;
    ReportSeriesV2UniformView series_view;
    if (is_series) {
        if (!report_series_payload_v2_uniform_view(chunk.payload,
                                                   chunk.payload_len,
                                                   chunk.record_count,
                                                   series_view) ||
            series_view.missing_bitmap_bytes != 0) {
            return ReportCacheCoalesceResult::InvalidSeries;
        }
    }

    const size_t incoming_bytes =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    const size_t fixed_bytes =
        is_series ? report_series_v2_header_size() : 0;

    size_t slot = AC_REPORT_COALESCE_SLOTS;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        if (!slots_[i].active ||
            slots_[i].kind != chunk.kind ||
            slots_[i].name != chunk.name) {
            continue;
        }

        CacheCoalesceBuffer &buf = slots_[i];
        if (!cache_coalesce_projected_size_fits(buf,
                                                incoming_bytes,
                                                fixed_bytes)) {
            continue;
        }
        if (buf.night_start_ms != night_start_ms) continue;
        if (is_series &&
            (buf.series_interval_ms != series_view.interval_ms ||
             chunk.start_ms != buf.last_ms)) {
            continue;
        }

        slot = i;
        break;
    }

    if (slot == AC_REPORT_COALESCE_SLOTS) {
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            if (!slots_[i].active) {
                slot = i;
                break;
            }
        }

        if (slot == AC_REPORT_COALESCE_SLOTS) {
            const CacheFlushResult flush = flush_slot(0, sink);
            if (flush == CacheFlushResult::Blocked) {
                return ReportCacheCoalesceResult::Backpressure;
            }
            if (flush == CacheFlushResult::Failed) {
                return ReportCacheCoalesceResult::FlushFailed;
            }
            slot = 0;
        }

        CacheCoalesceBuffer &buf = slots_[slot];
        buf.active = true;
        buf.kind = chunk.kind;
        buf.source = chunk.source;
        buf.name = chunk.name;
        buf.night_start_ms = night_start_ms;
        buf.first_ms = chunk.start_ms;
        buf.last_ms = chunk.end_ms;
        buf.record_count = 0;
        buf.payload_schema = chunk.payload_schema;
        buf.series_interval_ms = is_series ? series_view.interval_ms : 0;
        buf.series_values_pending = is_series;
        buf.payload.clear();
        buf.payload.set_max_size(AC_REPORT_COALESCE_TARGET_BYTES + 65536);
    }

    CacheCoalesceBuffer &buf = slots_[slot];
    const uint8_t *append_data =
        is_series ? series_view.values_milli_le : chunk.payload;
    const size_t append_len =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    if (!buf.payload.append(append_data, append_len)) {
        return ReportCacheCoalesceResult::PayloadAllocFailed;
    }

    if (chunk.start_ms < buf.first_ms) buf.first_ms = chunk.start_ms;
    if (chunk.end_ms > buf.last_ms) buf.last_ms = chunk.end_ms;
    buf.record_count += chunk.record_count;

    sink.note_cache_chunk_coverage(chunk);
    return ReportCacheCoalesceResult::Buffered;
}

CacheFlushResult ReportCacheCoalescer::flush_all(
    ReportCacheCoalescerSink &sink) {
    if (!slots_) return CacheFlushResult::Flushed;

    CacheFlushResult result = CacheFlushResult::Flushed;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        const CacheFlushResult flush = flush_slot(i, sink);
        if (flush == CacheFlushResult::Failed) return flush;
        if (flush == CacheFlushResult::Blocked) result = CacheFlushResult::Blocked;
    }

    return result;
}

void ReportCacheCoalescer::discard() {
    if (!slots_) return;

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        reset_slot(slots_[i]);
    }
}

CacheFlushResult ReportCacheCoalescer::flush_slot(
    size_t slot,
    ReportCacheCoalescerSink &sink) {
    if (!slots_ || slot >= AC_REPORT_COALESCE_SLOTS) {
        return CacheFlushResult::Flushed;
    }

    CacheCoalesceBuffer &buf = slots_[slot];
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
                    sink.enqueue_cache_write(buf, key, meta);
                if (queued == CacheWriteEnqueueResult::Blocked) {
                    return CacheFlushResult::Blocked;
                }
                if (queued == CacheWriteEnqueueResult::Failed) {
                    result = CacheFlushResult::Failed;
                }
            }
        }
    }

    reset_slot(buf);
    return result;
}

void ReportCacheCoalescer::reset_slot(CacheCoalesceBuffer &buf) {
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

}  // namespace aircannect
