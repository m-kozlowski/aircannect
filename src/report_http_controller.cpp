#include "report_http_controller.h"

#include "http_route_registry.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <limits.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "async_prepared_response.h"
#include "json_util.h"
#include "night_catalog.h"
#include "report_signal_store.h"
#include "report_task.h"
#include "runtime_clock.h"
#include "storage_stream_port.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr size_t REPORT_HTTP_ETAG_BYTES = 128;
constexpr size_t REPORT_HTTP_PENDING_CAPACITY = 6;
constexpr uint32_t REPORT_HTTP_PENDING_TIMEOUT_MS = 30000;
constexpr size_t REPORT_COMPLETION_JSON_RESERVE = 320;

constexpr const char *REPORT_SOURCE_REVISION_HEADER =
    "X-Report-Source-Revision";
constexpr const char *REPORT_GENERATION_HEADER = "X-Report-Generation";
constexpr const char *REPORT_TRACK_HEADER = "X-Report-Track";
constexpr const char *REPORT_LEVEL_HEADER = "X-Report-Level";
constexpr const char *REPORT_FIRST_BLOCK_HEADER = "X-Report-First-Block";
constexpr const char *REPORT_BLOCK_COUNT_HEADER = "X-Report-Block-Count";
constexpr const char *REPORT_PRESENT_BLOCKS_HEADER =
    "X-Report-Present-Blocks";
constexpr const char *REPORT_INTERVAL_HEADER = "X-Report-Interval-Ms";
constexpr const char *REPORT_SCALE_HEADER = "X-Report-Value-Scale";
constexpr const char *REPORT_OFFSET_HEADER = "X-Report-Value-Offset";
constexpr const char *REPORT_PHASE_HEADER = "X-Report-Grid-Phase-Ms";
constexpr const char *REPORT_ENVELOPE_HEADER = "X-Report-Envelope";

enum class PendingKind : uint8_t {
    Events,
    Signal,
};

void send_json_error(AsyncWebServerRequest *request,
                     int status,
                     const char *error) {
    if (!request) return;

    char body[160] = {};
    snprintf(body,
             sizeof(body),
             "{\"ok\":false,\"error\":\"%s\"}",
             error ? error : "error");
    request->send(status, "application/json", body);
}

void send_preparing(AsyncWebServerRequest *request) {
    if (!request) return;

    AsyncWebServerResponse *response = request->beginResponse(
        202,
        "application/json",
        "{\"ok\":true,\"state\":\"preparing\"}");
    if (!response) {
        request->send(202,
                      "application/json",
                      "{\"ok\":true,\"state\":\"preparing\"}");
        return;
    }

    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Retry-After", "1");
    request->send(response);
}

void send_night_failure(AsyncWebServerRequest *request,
                        const ReportNightFailureStatus &failure) {
    if (!request || !failure.valid()) return;

    char body[192] = {};
    snprintf(body,
             sizeof(body),
             "{\"ok\":false,\"state\":\"failed\",\"error\":\"%s\"}",
             failure.error);
    const int status = failure.retryable ? 503 : 404;
    AsyncWebServerResponse *response = request->beginResponse(
        status, "application/json", body);
    if (!response) {
        request->send(status, "application/json", body);
        return;
    }

    response->addHeader("Cache-Control", "no-store");
    if (failure.retryable) {
        char retry_after[12] = {};
        snprintf(retry_after,
                 sizeof(retry_after),
                 "%lu",
                 static_cast<unsigned long>(
                     (failure.retry_after_ms + 999) / 1000));
        response->addHeader("Retry-After", retry_after);
    }
    request->send(response);
}

bool parse_sleep_day(AsyncWebServerRequest *request, SleepDayId &sleep_day) {
    sleep_day = {};
    if (!request || !request->hasArg("night")) return false;

    const String value = request->arg("night");
    return value.length() == 8 &&
           SleepDayId::from_yyyymmdd(value.c_str(), sleep_day);
}

bool parse_uint64_arg(AsyncWebServerRequest *request,
                      const char *name,
                      uint64_t maximum,
                      uint64_t &value) {
    value = 0;
    if (!request || !name || !request->hasArg(name)) return false;

    const String text = request->arg(name);
    if (!text.length()) return false;

    uint64_t parsed = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char ch = text.charAt(i);
        if (ch < '0' || ch > '9') return false;

        const uint8_t digit = static_cast<uint8_t>(ch - '0');
        if (parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }

    value = parsed;
    return true;
}

bool parse_level(AsyncWebServerRequest *request,
                 ReportSignalStoreLevel &level,
                 const char *&name) {
    level = ReportSignalStoreLevel::Raw;
    name = "raw";
    if (!request || !request->hasArg("level")) return true;

    const String value = request->arg("level");
    if (value == "raw") return true;
    if (value == "1s") {
        level = ReportSignalStoreLevel::OneSecond;
        name = "1s";
        return true;
    }
    if (value == "10s") {
        level = ReportSignalStoreLevel::TenSeconds;
        name = "10s";
        return true;
    }
    return false;
}

bool request_etag_matches(AsyncWebServerRequest *request,
                          const char *etag) {
    if (!request || !etag || !etag[0] ||
        !request->hasHeader("If-None-Match")) {
        return false;
    }

    String values = request->getHeader("If-None-Match")->value();
    int start = 0;
    while (start < static_cast<int>(values.length())) {
        int end = values.indexOf(',', start);
        if (end < 0) end = values.length();

        String candidate = values.substring(start, end);
        candidate.trim();
        if (candidate == "*" || candidate == etag) return true;
        start = end + 1;
    }
    return false;
}

void add_common_headers(AsyncWebServerResponse *response,
                        const char *etag,
                        SourceRevision source_revision,
                        uint32_t generation) {
    if (!response) return;

    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Accept-Ranges", "none");
    if (etag && etag[0]) response->addHeader("ETag", etag);

    if (source_revision.valid()) {
        char revision[17] = {};
        snprintf(revision,
                 sizeof(revision),
                 "%016llx",
                 static_cast<unsigned long long>(source_revision.value()));
        response->addHeader(REPORT_SOURCE_REVISION_HEADER, revision);
    }
    if (generation != 0) {
        char value[12] = {};
        snprintf(value,
                 sizeof(value),
                 "%lu",
                 static_cast<unsigned long>(generation));
        response->addHeader(REPORT_GENERATION_HEADER, value);
    }
}

void add_signal_headers(AsyncWebServerResponse *response,
                        uint16_t track_index,
                        const char *level,
                        int64_t first_block_start_ms,
                        size_t block_count,
                        const char *present_blocks,
                        uint32_t interval_ms,
                        float value_scale,
                        float value_offset,
                        uint32_t grid_phase_ms,
                        bool envelope) {
    if (!response) return;

    char number[32] = {};
    snprintf(number, sizeof(number), "%u", track_index);
    response->addHeader(REPORT_TRACK_HEADER, number);
    response->addHeader(REPORT_LEVEL_HEADER, level ? level : "raw");
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(first_block_start_ms));
    response->addHeader(REPORT_FIRST_BLOCK_HEADER, number);
    snprintf(number,
             sizeof(number),
             "%u",
             static_cast<unsigned>(block_count));
    response->addHeader(REPORT_BLOCK_COUNT_HEADER, number);
    response->addHeader(
        REPORT_PRESENT_BLOCKS_HEADER, present_blocks ? present_blocks : "");
    snprintf(number, sizeof(number), "%lu",
             static_cast<unsigned long>(interval_ms));
    response->addHeader(REPORT_INTERVAL_HEADER, number);
    snprintf(number, sizeof(number), "%.9g", static_cast<double>(value_scale));
    response->addHeader(REPORT_SCALE_HEADER, number);
    snprintf(number, sizeof(number), "%.9g", static_cast<double>(value_offset));
    response->addHeader(REPORT_OFFSET_HEADER, number);
    snprintf(number, sizeof(number), "%lu",
             static_cast<unsigned long>(grid_phase_ms));
    response->addHeader(REPORT_PHASE_HEADER, number);
    response->addHeader(REPORT_ENVELOPE_HEADER, envelope ? "1" : "0");
}

void send_not_modified(AsyncWebServerRequest *request,
                       const char *etag,
                       SourceRevision source_revision,
                       uint32_t generation) {
    AsyncWebServerResponse *response = request->beginResponse(304);
    if (!response) {
        request->send(304);
        return;
    }

    add_common_headers(response, etag, source_revision, generation);
    request->send(response);
}

bool format_night_etag(const ReportNightQuery &query,
                       char *out,
                       size_t out_size) {
    char day[9] = {};
    if (query.state != ReportStoreQueryState::Ready ||
        !query.sleep_day.format_yyyymmdd(day, sizeof(day))) {
        return false;
    }

    const int written = snprintf(
        out,
        out_size,
        "\"night-%s-%016llx-%08lx\"",
        day,
        static_cast<unsigned long long>(query.source_revision.value()),
        static_cast<unsigned long>(query.generation));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool format_events_etag(const ReportEventFileQuery &query,
                        char *out,
                        size_t out_size) {
    char day[9] = {};
    if (query.state != ReportStoreQueryState::Ready ||
        !query.sleep_day.format_yyyymmdd(day, sizeof(day))) {
        return false;
    }

    const int written = snprintf(
        out,
        out_size,
        "\"events-%s-%016llx-%08lx\"",
        day,
        static_cast<unsigned long long>(query.source_revision.value()),
        static_cast<unsigned long>(query.generation));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool format_signal_etag(const ReportSignalRangeQuery &query,
                        char *out,
                        size_t out_size) {
    char day[9] = {};
    if (query.state != ReportStoreQueryState::Ready ||
        !query.track.sleep_day.format_yyyymmdd(day, sizeof(day))) {
        return false;
    }

    const int written = snprintf(
        out,
        out_size,
        "\"signal-%s-%016llx-%08lx-%u-%u-%lu-%u-%lld-%u\"",
        day,
        static_cast<unsigned long long>(
            query.track.source_revision.value()),
        static_cast<unsigned long>(query.track.generation),
        static_cast<unsigned>(query.track.signal),
        static_cast<unsigned>(query.track.track_index),
        static_cast<unsigned long>(query.track.sample_interval_ms),
        static_cast<unsigned>(query.level),
        static_cast<long long>(query.first_block_start_ms),
        static_cast<unsigned>(query.block_count));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

void append_session_json(LargeTextBuffer &json,
                         const NightCatalogTimeRange &session) {
    char number[32] = {};

    json += "{\"start\":";
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(session.start_ms));
    json += number;
    json += ",\"end\":";
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(session.end_ms));
    json += number;
    json += ",\"duration_min\":";
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(
                 (session.end_ms - session.start_ms) / 60000));
    json += number;
    json += '}';
}

uint64_t catalog_identity(const NightCatalog &catalog,
                          const ReportSignalStoreCatalog *store) {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t value) {
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8));
            hash *= 1099511628211ULL;
        }
    };

    mix(catalog.size());
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *night = catalog.record(i);
        if (!night) continue;

        mix(static_cast<uint32_t>(night->sleep_day.epoch_days()));
        mix(night->source_revision.value());
        const ReportSignalStoreCatalogRecord *stored = store
            ? store->find(night->sleep_day) : nullptr;
        mix(stored && stored->source_revision == night->source_revision
                ? stored->generation : 0);
    }
    return hash;
}

bool format_catalog_etag(const NightCatalog &catalog,
                         const ReportSignalStoreCatalog *store,
                         char *out,
                         size_t out_size) {
    const int written = snprintf(
        out,
        out_size,
        "\"catalog-%016llx\"",
        static_cast<unsigned long long>(catalog_identity(catalog, store)));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool report_task_available(AsyncWebServerRequest *request,
                           const ReportTask &report_task) {
    const ReportTaskControlSnapshot status = report_task.control_snapshot();
    if (!status.initialized) {
        send_json_error(request, 503, "report_unavailable");
        return false;
    }
    if (!status.task_started) {
        send_preparing(request);
        return false;
    }
    return true;
}

bool begin_missing_night(AsyncWebServerRequest *request,
                         ReportTask &report_task,
                         SleepDayId sleep_day,
                         uint32_t generation) {
    ReportNightFailureStatus failure;
    if (report_task.night_failure(sleep_day, failure)) {
        send_night_failure(request, failure);
        return false;
    }

    const OperationAdmission admitted = report_task.request_night(
        sleep_day,
        ReportRequestPriority::Foreground,
        generation,
        false);
    if (admitted == OperationAdmission::Rejected) {
        send_json_error(request, 503, "report_queue_unavailable");
        return false;
    }

    send_preparing(request);
    return false;
}

bool handle_query_state(AsyncWebServerRequest *request,
                        ReportTask &report_task,
                        SleepDayId sleep_day,
                        ReportStoreQueryState state,
                        uint32_t generation) {
    switch (state) {
        case ReportStoreQueryState::Ready:
            return true;
        case ReportStoreQueryState::Unavailable:
            send_json_error(request, 503, "report_unavailable");
            return false;
        case ReportStoreQueryState::CatalogPending:
            send_preparing(request);
            return false;
        case ReportStoreQueryState::NightMissing:
            send_json_error(request, 404, "no_such_night");
            return false;
        case ReportStoreQueryState::StorePending:
            return begin_missing_night(
                request, report_task, sleep_day, generation);
        case ReportStoreQueryState::TrackMissing:
            send_json_error(request, 404, "signal_not_available");
            return false;
        case ReportStoreQueryState::InvalidRange:
            send_json_error(request, 400, "bad_signal_range");
            return false;
    }
    send_json_error(request, 500, "report_query_invalid");
    return false;
}

size_t track_first_slot(const ReportSignalStoreTrack &track,
                        int64_t first_block_start_ms) {
    return static_cast<size_t>(
        (first_block_start_ms - track.first_block_start_ms) /
        REPORT_SIGNAL_STORE_BLOCK_MS);
}

bool track_block_present(const ReportSignalStoreTrack &track, size_t slot) {
    return slot < track.block_slot_count &&
        (track.present_blocks[slot / 8] & (1u << (slot % 8))) != 0;
}

bool format_present_blocks(const ReportSignalRangeQuery &query,
                           char *out,
                           size_t out_size) {
    if (!out || out_size <= query.block_count) return false;

    const size_t first_slot = track_first_slot(
        query.track, query.first_block_start_ms);
    for (size_t i = 0; i < query.block_count; ++i) {
        out[i] = track_block_present(query.track, first_slot + i) ? '1' : '0';
    }
    out[query.block_count] = '\0';
    return true;
}

struct HttpStreamRef {
    StorageStreamPort *port = nullptr;
    std::shared_ptr<StorageByteStream> stream;
    size_t size = 0;
    bool finished = false;

    ~HttpStreamRef() { finish(false); }

    void finish(bool complete) {
        if (finished || !port || !stream) return;
        port->finish(*stream, complete);
        finished = true;
    }
};

}  // namespace

struct ReportHttpController::PendingResponses {
    struct Entry {
        PendingKind kind = PendingKind::Events;
        AsyncWebServerRequestPtr request;
        std::shared_ptr<StorageByteStream> stream;
        uint32_t deadline_ms = 0;
        uint64_t response_size = 0;
        SourceRevision source_revision;
        uint32_t generation = 0;
        uint16_t track_index = 0;
        uint32_t interval_ms = 0;
        float value_scale = 0;
        float value_offset = 0;
        uint32_t grid_phase_ms = 0;
        int64_t first_block_start_ms = 0;
        size_t block_count = 0;
        bool envelope = false;
        char etag[REPORT_HTTP_ETAG_BYTES] = {};
        char level[8] = {};
        char present_blocks[REPORT_SIGNAL_STORE_MAX_BLOCKS + 1] = {};

        bool used() const { return static_cast<bool>(stream); }
    };

    StaticSemaphore_t mutex_storage = {};
    SemaphoreHandle_t mutex = nullptr;
    Entry entries[REPORT_HTTP_PENDING_CAPACITY] = {};
};

ReportHttpController::ReportHttpController() = default;
ReportHttpController::~ReportHttpController() = default;

void ReportHttpController::register_routes(HttpRouteRegistry &server) {
    server.on(AsyncURIMatcher::exact("/api/report/summary"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_summary(request);
    });
    server.on(AsyncURIMatcher::exact("/api/report/result"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_result(request);
    });
    server.on(AsyncURIMatcher::exact("/api/report/plot"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_plot(request);
    });
}

void ReportHttpController::begin(ReportTask &report_task,
                                 StorageStreamPort &stream_port) {
    report_task_ = &report_task;
    stream_port_ = &stream_port;
    observed_completion_ = {};
    completion_serial_ = 0;

    if (completion_snapshot_.begin(REPORT_COMPLETION_JSON_RESERVE) &&
        completion_json_.reserve(REPORT_COMPLETION_JSON_RESERVE)) {
        completion_json_ = "{\"serial\":0}";
        (void)completion_snapshot_.replace(completion_json_);
    }

    if (!pending_) pending_.reset(new (std::nothrow) PendingResponses());
    if (pending_ && !pending_->mutex) {
        pending_->mutex =
            xSemaphoreCreateMutexStatic(&pending_->mutex_storage);
    }
}

void ReportHttpController::poll() {
    publish_completion();

    if (!stream_port_ || !pending_ || !pending_->mutex ||
        xSemaphoreTake(pending_->mutex, 0) != pdTRUE) {
        return;
    }

    const uint32_t now_ms = millis();
    for (PendingResponses::Entry &entry : pending_->entries) {
        if (!entry.used()) continue;

        if (entry.request.expired()) {
            stream_port_->finish(*entry.stream, false);
            entry = {};
            continue;
        }

        StorageStreamStatus status;
        if (!stream_port_->status(*entry.stream, status)) continue;
        const bool timed_out =
            millis_deadline_reached(now_ms, entry.deadline_ms);
        if (status.state == StorageStreamState::Preparing && !timed_out) {
            continue;
        }

        const AsyncWebServerRequestPtr pending_request = entry.request;
        PendingResponses::Entry ready = std::move(entry);
        entry = {};
        xSemaphoreGive(pending_->mutex);

        std::shared_ptr<AsyncWebServerRequest> request = pending_request.lock();
        if (!request) {
            stream_port_->finish(*ready.stream, false);
            return;
        }
        if (timed_out) {
            stream_port_->finish(*ready.stream, false);
            send_json_error(request.get(), 503, "report_stream_timeout");
            return;
        }
        if (status.state == StorageStreamState::Error ||
            status.state == StorageStreamState::Cancelled) {
            stream_port_->finish(*ready.stream, false);
            send_json_error(request.get(),
                            503,
                            status.error[0]
                                ? status.error
                                : "report_stream_failed");
            return;
        }
        if (status.state != StorageStreamState::Ready ||
            status.size != ready.response_size ||
            !stream_port_->attach(*ready.stream)) {
            stream_port_->finish(*ready.stream, false);
            send_json_error(request.get(), 503, "report_stream_unavailable");
            return;
        }

        std::shared_ptr<HttpStreamRef> ref = std::make_shared<HttpStreamRef>();
        if (!ref) {
            stream_port_->finish(*ready.stream, false);
            send_json_error(request.get(), 503, "response_alloc");
            return;
        }
        ref->port = stream_port_;
        ref->stream = std::move(ready.stream);
        ref->size = static_cast<size_t>(ready.response_size);

        AsyncWebServerResponse *response = new (std::nothrow)
            AsyncPreparedResponse(
                "application/octet-stream",
                static_cast<size_t>(ready.response_size),
                [ref](uint8_t *buffer,
                      size_t max_length,
                      size_t offset) -> size_t {
                    if (!buffer || !ref || !ref->port || !ref->stream) {
                        return 0;
                    }

                    const StorageStreamRead read = ref->port->read(
                        *ref->stream, buffer, max_length, offset);
                    if (read.state == StorageStreamReadState::Retry) {
                        return RESPONSE_TRY_AGAIN;
                    }
                    if (read.state != StorageStreamReadState::Data) return 0;
                    if (offset + read.bytes >= ref->size) {
                        ref->finish(true);
                    }
                    return read.bytes;
                });
        if (!response) {
            ref->finish(false);
            send_json_error(request.get(), 503, "response_alloc");
            return;
        }

        add_common_headers(response,
                           ready.etag,
                           ready.source_revision,
                           ready.generation);
        if (ready.kind == PendingKind::Signal) {
            add_signal_headers(response,
                               ready.track_index,
                               ready.level,
                               ready.first_block_start_ms,
                               ready.block_count,
                               ready.present_blocks,
                               ready.interval_ms,
                               ready.value_scale,
                               ready.value_offset,
                               ready.grid_phase_ms,
                               ready.envelope);
        }
        request->send(response);
        return;
    }

    xSemaphoreGive(pending_->mutex);
}

void ReportHttpController::send_summary(
    AsyncWebServerRequest *request) const {
    if (!report_task_ || !report_task_available(request, *report_task_)) return;

    const std::shared_ptr<const NightCatalog> catalog =
        report_task_->catalog_snapshot();
    if (!catalog) {
        send_preparing(request);
        return;
    }
    const std::shared_ptr<const ReportSignalStoreCatalog> store =
        report_task_->store_catalog_snapshot();

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    if (format_catalog_etag(*catalog, store.get(), etag, sizeof(etag)) &&
        request_etag_matches(request, etag)) {
        send_not_modified(request, etag, {}, 0);
        return;
    }

    std::shared_ptr<LargeTextBuffer> json =
        std::make_shared<LargeTextBuffer>();
    if (!json || !json->reserve(256 + catalog->size() * 288)) {
        send_json_error(request, 503, "summary_alloc");
        return;
    }

    const ReportTaskControlSnapshot status =
        report_task_->control_snapshot();
    char number[32] = {};
    *json = "{\"state\":\"ready\",\"generation\":";
    snprintf(number,
             sizeof(number),
             "%lu",
             static_cast<unsigned long>(status.catalog_generation));
    *json += number;
    *json += ",\"nights\":[";
    for (size_t i = 0; i < catalog->size(); ++i) {
        const NightCatalogRecord *night = catalog->record(i);
        if (!night) continue;

        if (i) *json += ',';
        char day[9] = {};
        night->sleep_day.format_yyyymmdd(day, sizeof(day));
        const ReportSignalStoreCatalogRecord *stored = store
            ? store->find(night->sleep_day) : nullptr;
        const bool materialized = stored &&
            stored->source_revision == night->source_revision;

        *json += "{\"id\":\"";
        *json += day;
        *json += "\",\"start\":";
        snprintf(number,
                 sizeof(number),
                 "%lld",
                 static_cast<long long>(night->day_start_ms));
        *json += number;
        *json += ",\"end\":";
        snprintf(number,
                 sizeof(number),
                 "%lld",
                 static_cast<long long>(night->day_end_ms));
        *json += number;
        *json += ",\"duration_min\":";
        snprintf(number,
                 sizeof(number),
                 "%lu",
                 static_cast<unsigned long>(
                     night_catalog_duration_minutes(*catalog, *night)));
        *json += number;
        *json += ",\"materialized\":";
        *json += materialized ? "true" : "false";
        *json += ",\"report_generation\":";
        snprintf(number,
                 sizeof(number),
                 "%lu",
                 static_cast<unsigned long>(
                     materialized ? stored->generation : 0));
        *json += number;
        *json += ",\"sessions\":[";

        size_t session_count = 0;
        const NightCatalogTimeRange *sessions =
            catalog->sessions(*night, session_count);
        for (size_t session = 0; sessions && session < session_count;
             ++session) {
            if (session) *json += ',';
            append_session_json(*json, sessions[session]);
        }
        *json += "]}";
    }
    *json += "]}";

    if (json->overflowed()) {
        send_json_error(request, 503, "summary_alloc");
        return;
    }

    AsyncWebServerResponse *response = new (std::nothrow)
        AsyncPreparedResponse(
            "application/json",
            json->length(),
            [json](uint8_t *buffer,
                   size_t max_length,
                   size_t offset) -> size_t {
                if (!buffer || offset >= json->length()) return 0;

                const size_t copied = std::min(
                    max_length, json->length() - offset);
                memcpy(buffer, json->c_str() + offset, copied);
                return copied;
            });
    if (!response) {
        send_json_error(request, 503, "response_alloc");
        return;
    }

    add_common_headers(response, etag, {}, 0);
    request->send(response);
}

void ReportHttpController::send_result(AsyncWebServerRequest *request) {
    if (!report_task_ || !report_task_available(request, *report_task_)) return;

    SleepDayId sleep_day;
    if (!parse_sleep_day(request, sleep_day)) {
        send_json_error(request, 400, "bad_night");
        return;
    }

    const ReportNightQuery query = report_task_->query_night(sleep_day);
    if (!handle_query_state(request,
                            *report_task_,
                            sleep_day,
                            query.state,
                            next_generation())) {
        return;
    }

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    (void)format_night_etag(query, etag, sizeof(etag));
    if (request_etag_matches(request, etag)) {
        send_not_modified(
            request, etag, query.source_revision, query.generation);
        return;
    }

    const std::shared_ptr<const LargeByteBuffer> metadata = query.metadata;
    AsyncWebServerResponse *response = new (std::nothrow)
        AsyncPreparedResponse(
            "application/octet-stream",
            metadata->size(),
            [metadata](uint8_t *buffer,
                       size_t max_length,
                       size_t offset) -> size_t {
                if (!buffer || offset >= metadata->size()) return 0;

                const size_t copied = std::min(
                    max_length, metadata->size() - offset);
                memcpy(buffer, metadata->data() + offset, copied);
                return copied;
            });
    if (!response) {
        send_json_error(request, 503, "response_alloc");
        return;
    }

    add_common_headers(
        response, etag, query.source_revision, query.generation);
    request->send(response);
}

void ReportHttpController::send_plot(AsyncWebServerRequest *request) {
    if (!report_task_ || !stream_port_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }
    if (!report_task_available(request, *report_task_)) {
        return;
    }

    SleepDayId sleep_day;
    if (!parse_sleep_day(request, sleep_day)) {
        send_json_error(request, 400, "bad_night");
        return;
    }
    if (!request->hasArg("part")) {
        send_json_error(request, 400, "missing_plot_part");
        return;
    }

    PendingResponses::Entry pending;
    StorageStreamCommand command;
    command.lane = StorageStreamLane::Foreground;
    command.verification = StorageStreamVerification::Size;

    const String part = request->arg("part");
    if (part == "events") {
        const ReportEventFileQuery query =
            report_task_->query_events(sleep_day);
        if (!handle_query_state(request,
                                *report_task_,
                                sleep_day,
                                query.state,
                                next_generation())) {
            return;
        }

        pending.kind = PendingKind::Events;
        pending.response_size = query.file_size;
        pending.source_revision = query.source_revision;
        pending.generation = query.generation;
        (void)format_events_etag(query, pending.etag, sizeof(pending.etag));

        if (request_etag_matches(request, pending.etag)) {
            send_not_modified(request,
                              pending.etag,
                              query.source_revision,
                              query.generation);
            return;
        }

        command.path = query.path;
        command.expected_size = query.file_size;
        command.source_length = query.file_size;
    } else if (part == "signal") {
        uint64_t track_index = 0;
        uint64_t from = 0;
        uint64_t to = 0;
        ReportSignalStoreLevel level;
        const char *level_name = nullptr;
        if (!parse_uint64_arg(request, "track", UINT16_MAX, track_index) ||
            !parse_uint64_arg(request, "from", INT64_MAX, from) ||
            !parse_uint64_arg(request, "to", INT64_MAX, to) ||
            to <= from ||
            from % REPORT_SIGNAL_STORE_BLOCK_MS != 0 ||
            to % REPORT_SIGNAL_STORE_BLOCK_MS != 0 ||
            !parse_level(request, level, level_name)) {
            send_json_error(request, 400, "bad_signal_request");
            return;
        }

        const uint64_t block_count =
            (to - from) / REPORT_SIGNAL_STORE_BLOCK_MS;
        if (block_count == 0 ||
            block_count > REPORT_SIGNAL_STORE_MAX_BLOCKS) {
            send_json_error(request, 400, "bad_signal_range");
            return;
        }

        const ReportSignalRangeQuery query = report_task_->query_signal(
            sleep_day,
            static_cast<size_t>(track_index),
            static_cast<int64_t>(from),
            static_cast<size_t>(block_count),
            level);
        if (!handle_query_state(request,
                                *report_task_,
                                sleep_day,
                                query.state,
                                next_generation())) {
            return;
        }

        pending.kind = PendingKind::Signal;
        pending.response_size = query.range.length;
        pending.source_revision = query.track.source_revision;
        pending.generation = query.track.generation;
        pending.track_index = query.track.track_index;
        pending.interval_ms = query.range.interval_ms;
        pending.value_scale = query.track.value_scale;
        pending.value_offset = query.track.value_offset;
        pending.grid_phase_ms = query.track.grid_phase_ms;
        pending.first_block_start_ms = query.first_block_start_ms;
        pending.block_count = query.block_count;
        pending.envelope = query.range.envelope;
        copy_cstr(pending.level, sizeof(pending.level), level_name);
        if (!format_present_blocks(
                query,
                pending.present_blocks,
                sizeof(pending.present_blocks)) ||
            !format_signal_etag(query, pending.etag, sizeof(pending.etag))) {
            send_json_error(request, 500, "signal_response_invalid");
            return;
        }

        if (request_etag_matches(request, pending.etag)) {
            send_not_modified(request,
                              pending.etag,
                              pending.source_revision,
                              pending.generation);
            return;
        }

        if (query.range.length == 0) {
            AsyncWebServerResponse *response = request->beginResponse(204);
            if (!response) {
                request->send(204);
                return;
            }
            add_common_headers(response,
                               pending.etag,
                               pending.source_revision,
                               pending.generation);
            add_signal_headers(response,
                               pending.track_index,
                               pending.level,
                               pending.first_block_start_ms,
                               pending.block_count,
                               pending.present_blocks,
                               pending.interval_ms,
                               pending.value_scale,
                               pending.value_offset,
                               pending.grid_phase_ms,
                               pending.envelope);
            request->send(response);
            return;
        }

        command.path = query.path;
        command.expected_size = query.file_size;
        // Appends may grow the file after this metadata snapshot was taken.
        // The selected closed range stays at the same offset and length.
        command.verification = StorageStreamVerification::None;
        command.source_offset = query.range.offset;
        command.source_length = query.range.length;
    } else {
        send_json_error(request, 400, "bad_plot_part");
        return;
    }

    char stream_error[AC_STORAGE_ERROR_MAX] = {};
    if (!stream_port_->request_stream(
            command,
            pending.stream,
            stream_error,
            sizeof(stream_error))) {
        send_json_error(request,
                        503,
                        stream_error[0]
                            ? stream_error
                            : "report_stream_unavailable");
        return;
    }

    if (!pending_ || !pending_->mutex ||
        xSemaphoreTake(pending_->mutex, 0) != pdTRUE) {
        stream_port_->finish(*pending.stream, false);
        send_preparing(request);
        return;
    }

    PendingResponses::Entry *slot = nullptr;
    for (PendingResponses::Entry &entry : pending_->entries) {
        if (!entry.used()) {
            slot = &entry;
            break;
        }
    }
    if (!slot) {
        xSemaphoreGive(pending_->mutex);
        stream_port_->finish(*pending.stream, false);
        send_json_error(request, 503, "report_stream_slots_full");
        return;
    }

    pending.request = request->pause();
    pending.deadline_ms = millis() + REPORT_HTTP_PENDING_TIMEOUT_MS;
    *slot = std::move(pending);
    xSemaphoreGive(pending_->mutex);
}

void ReportHttpController::publish_completion() {
    if (!report_task_) return;

    const ReportEngineCompletion completion = report_task_->last_completion();
    if (!completion.valid() ||
        completion.request.ticket == observed_completion_) {
        return;
    }

    observed_completion_ = completion.request.ticket;
    completion_serial_++;
    if (completion_serial_ == 0) completion_serial_ = 1;

    char day[9] = {};
    if (!completion.request.artifact.sleep_day.format_yyyymmdd(
            day, sizeof(day))) {
        return;
    }

    const bool success = completion.outcome.disposition ==
        OperationDisposition::Succeeded;
    const char *error = completion.error;
    if (!success && !error[0] &&
        completion.outcome.disposition == OperationDisposition::Cancelled) {
        error = "cancelled";
    }

    completion_json_.clear();
    completion_json_ = "{";
    json_add_uint64(completion_json_, "serial", completion_serial_, false);
    json_add_string(completion_json_, "night", day);
    json_add_string(completion_json_, "kind", "night");
    json_add_bool(completion_json_, "success", success);
    json_add_bool(completion_json_,
                  "forced",
                  completion.request.force_rebuild);
    json_add_uint64(completion_json_,
                    "generation",
                    completion.store_generation);
    json_add_string(completion_json_, "error", error);
    completion_json_ += '}';

    if (!completion_json_.overflowed()) {
        (void)completion_snapshot_.replace(completion_json_);
    }
}

uint32_t ReportHttpController::next_generation() const {
    uint32_t generation = next_generation_.fetch_add(
        1, std::memory_order_relaxed);
    if (generation != 0) return generation;

    generation = next_generation_.fetch_add(1, std::memory_order_relaxed);
    return generation == 0 ? 1 : generation;
}

}  // namespace aircannect
