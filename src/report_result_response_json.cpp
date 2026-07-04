#include "report_manager.h"

#include <algorithm>

#include "json_util.h"
#include "report_result_json.h"
#include "report_result_response_json.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

const char *report_provider_id_name(ReportProviderId provider) {
    switch (provider) {
        case ReportProviderId::Edf: return "edf";
        case ReportProviderId::Spool: return "spool";
        default: return "unknown";
    }
}

}  // namespace

void ReportResultBuildService::build_chunks_json(LargeTextBuffer &json,
                                                 size_t offset,
                                                 size_t limit) const {
    if (limit > 128) limit = 128;
    const size_t total = runtime_.status().chunk_count;
    if (offset > total) offset = total;
    const size_t end = std::min(total, offset + limit);

    json.clear();
    json += "{";
    json_add_string(json, "state", runtime_.state_name(), false);
    json_add_int(json, "offset", static_cast<long>(offset));
    json_add_int(json, "limit", static_cast<long>(limit));
    json_add_int(json, "total", static_cast<long>(total));
    json_add_bool(json, "more", end < total);
    json += ",\"chunks\":[";

    for (size_t i = offset; i < end; ++i) {
        const ReportResultChunk &chunk = runtime_.scratch().chunks()[i];
        if (i != offset) json += ',';

        json += "{";
        json_add_int(json, "id", static_cast<long>(i), false);
        json_add_string(json, "kind", ReportStore::kind_name(chunk.kind));
        json_add_string(json,
                        "source",
                        report_source_spool_type(chunk.source));
        json_add_string(json,
                        "provider",
                        report_provider_id_name(
                            chunk.provider_ref.provider));
        json_add_string(json, "name", chunk.name ? chunk.name : "");
        json_add_int(json,
                     "stream",
                     static_cast<long>(chunk.stream_index));
        json_add_uint64(json,
                        "start",
                        static_cast<uint64_t>(chunk.start_ms));
        json_add_uint64(json, "end", static_cast<uint64_t>(chunk.end_ms));
        json_add_int(json,
                     "schema",
                     static_cast<long>(chunk.payload_schema));
        json_add_int(json,
                     "records",
                     static_cast<long>(chunk.record_count));
        json_add_int(json,
                     "bytes",
                     static_cast<long>(chunk.payload_len));
        json += "}";
    }

    json += "]}";
}

void ReportManager::build_result_chunks_json(LargeTextBuffer &json,
                                             size_t offset,
                                             size_t limit) const {
    result_build_.build_chunks_json(json, offset, limit);
}

}  // namespace aircannect
