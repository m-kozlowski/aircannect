#include "report_http_controller.h"

#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "async_prepared_response.h"
#include "board_report.h"
#include "json_util.h"
#include "large_text_buffer.h"
#include "night_catalog.h"
#include "report_artifacts.h"
#include "report_range_tile.h"
#include "report_task.h"
#include "runtime_clock.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t REPORT_HTTP_ETAG_BYTES = 160;
static constexpr size_t REPORT_HTTP_PENDING_CAPACITY = 4;
static constexpr uint32_t REPORT_HTTP_PENDING_TIMEOUT_MS = 30000;
static constexpr const char *REPORT_SOURCE_REVISION_HEADER =
    "X-Report-Source-Revision";

bool request_accepts_deflate(AsyncWebServerRequest *request) {
    if (!request || !request->hasHeader("Accept-Encoding")) return false;

    String value = request->getHeader("Accept-Encoding")->value();
    value.toLowerCase();

    bool wildcard_seen = false;
    bool wildcard_enabled = false;
    int start = 0;
    while (start < static_cast<int>(value.length())) {
        int end = value.indexOf(',', start);
        if (end < 0) end = value.length();

        String item = value.substring(start, end);
        item.trim();
        const int separator = item.indexOf(';');
        String coding = separator < 0
            ? item
            : item.substring(0, separator);
        coding.trim();
        bool enabled = true;
        if (separator >= 0) {
            String parameters = item.substring(separator + 1);
            parameters.replace(" ", "");
            const int quality = parameters.indexOf("q=");
            enabled = quality < 0 ||
                parameters.substring(quality + 2).toFloat() > 0.0f;
        }

        if (coding == "deflate") return enabled;
        if (coding == "*") {
            wildcard_seen = true;
            wildcard_enabled = enabled;
        }
        start = end + 1;
    }
    return wildcard_seen && wildcard_enabled;
}

bool same_artifact_descriptor(const ReportArtifactDescriptor &lhs,
                              const ReportArtifactDescriptor &rhs) {
    return lhs.key == rhs.key && lhs.size == rhs.size &&
           lhs.crc32 == rhs.crc32;
}

bool same_payload_descriptor(
    const ReportArtifactPayloadDescriptor &lhs,
    const ReportArtifactPayloadDescriptor &rhs) {
    return lhs == rhs;
}

void send_json_error(AsyncWebServerRequest *request,
                     int status,
                     const char *error) {
    if (!request) return;

    char body[128] = {};
    snprintf(body,
             sizeof(body),
             "{\"ok\":false,\"error\":\"%s\"}",
             error ? error : "error");
    request->send(status, "application/json", body);
}

void send_preparing(AsyncWebServerRequest *request) {
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

void send_artifact_failure(
    AsyncWebServerRequest *request,
    const ReportArtifactFailureStatus &failure) {
    if (!request || !failure.valid()) return;

    char body[160] = {};
    snprintf(body,
             sizeof(body),
             "{\"ok\":false,\"state\":\"failed\",\"error\":\"%s\"}",
             failure.error);
    AsyncWebServerResponse *response = request->beginResponse(
        503, "application/json", body);
    if (!response) {
        request->send(503, "application/json", body);
        return;
    }

    char retry_after[12] = {};
    snprintf(retry_after,
             sizeof(retry_after),
             "%lu",
             static_cast<unsigned long>(
                 (failure.retry_after_ms + 999) / 1000));
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Retry-After", retry_after);
    request->send(response);
}

bool parse_sleep_day(AsyncWebServerRequest *request, SleepDayId &sleep_day) {
    sleep_day = {};
    if (!request || !request->hasArg("night")) return false;

    const String value = request->arg("night");
    return value.length() == 8 &&
           SleepDayId::from_yyyymmdd(value.c_str(), sleep_day);
}

bool parse_positive_int64(AsyncWebServerRequest *request,
                          const char *name,
                          int64_t &value) {
    value = 0;
    if (!request || !name || !request->hasArg(name)) return false;

    const String text = request->arg(name);
    if (!text.length()) return false;

    uint64_t parsed = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char ch = text.charAt(i);
        if (ch < '0' || ch > '9') return false;

        const uint8_t digit = static_cast<uint8_t>(ch - '0');
        if (parsed > (static_cast<uint64_t>(INT64_MAX) - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }

    value = static_cast<int64_t>(parsed);
    return value > 0;
}

bool format_artifact_etag(const ReportArtifactDescriptor &artifact,
                          char *out,
                          size_t out_size) {
    if (!artifact.valid() || !out || out_size == 0) return false;

    char day[9] = {};
    if (!artifact.key.sleep_day.format_yyyymmdd(day, sizeof(day))) {
        return false;
    }

    char kind = 'r';
    if (artifact.key.kind == ReportArtifactKind::Overview) kind = 'o';
    if (artifact.key.kind == ReportArtifactKind::RangeTile) kind = 't';

    int written = 0;
    if (artifact.key.kind == ReportArtifactKind::RangeTile) {
        written = snprintf(
            out,
            out_size,
            "W/\"%c-%s-%016llx-%lld-%llu-%08lx\"",
            kind,
            day,
            static_cast<unsigned long long>(
                artifact.key.source_revision.value()),
            static_cast<long long>(artifact.key.range_start_ms),
            static_cast<unsigned long long>(artifact.size),
            static_cast<unsigned long>(artifact.crc32));
    } else {
        written = snprintf(
            out,
            out_size,
            "W/\"%c-%s-%016llx-%llu-%08lx\"",
            kind,
            day,
            static_cast<unsigned long long>(
                artifact.key.source_revision.value()),
            static_cast<unsigned long long>(artifact.size),
            static_cast<unsigned long>(artifact.crc32));
    }
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool format_payload_etag(
    const ReportArtifactPayloadDescriptor &payload,
    char *out,
    size_t out_size) {
    if (!payload.valid() || !out || out_size == 0) return false;
    if (payload.is_whole()) {
        return format_artifact_etag(payload.artifact, out, out_size);
    }

    char day[9] = {};
    if (!payload.artifact.key.sleep_day.format_yyyymmdd(
            day, sizeof(day))) {
        return false;
    }

    const ReportArtifactKey &key = payload.artifact.key;
    const int written = snprintf(
        out,
        out_size,
        "W/\"p-%s-%016llx-%u-%lld-%lu-%lu-%08lx\"",
        day,
        static_cast<unsigned long long>(key.source_revision.value()),
        static_cast<unsigned>(payload.kind),
        static_cast<long long>(key.range_start_ms),
        static_cast<unsigned long>(payload.offset),
        static_cast<unsigned long>(payload.size),
        static_cast<unsigned long>(payload.crc32));
    return written > 0 && static_cast<size_t>(written) < out_size;
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
        if (candidate == "*" || candidate == etag ||
            (candidate.startsWith("W/") &&
             candidate.substring(2) == etag)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

void add_artifact_headers(AsyncWebServerResponse *response,
                          const char *etag,
                          SourceRevision source_revision = {}) {
    if (!response) return;

    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Accept-Ranges", "none");
    response->addHeader("Vary", "Accept-Encoding");
    if (etag && etag[0]) response->addHeader("ETag", etag);

    if (source_revision.valid()) {
        char revision[17] = {};
        snprintf(revision,
                 sizeof(revision),
                 "%016llx",
                 static_cast<unsigned long long>(source_revision.value()));
        response->addHeader(REPORT_SOURCE_REVISION_HEADER, revision);
    }
}

void send_not_modified(AsyncWebServerRequest *request,
                       const char *etag,
                       SourceRevision source_revision = {}) {
    AsyncWebServerResponse *response = request->beginResponse(304);
    if (!response) {
        request->send(304);
        return;
    }

    add_artifact_headers(response, etag, source_revision);
    request->send(response);
}

bool send_artifact_payload(
    AsyncWebServerRequest *request,
    const ReportArtifactPayloadDescriptor &descriptor,
    ReportArtifactPayloadSelection payload,
    const char *content_type) {
    if (!request || !descriptor.valid() || !payload.ready()) {
        return false;
    }

    const bool deflated = payload.encoding ==
        ReportArtifactPayloadEncoding::Deflate;
    if ((!deflated && payload.bytes->size() != descriptor.size) ||
        (deflated && payload.bytes->size() >= descriptor.size)) {
        return false;
    }

    std::shared_ptr<const LargeByteBuffer> bytes =
        std::move(payload.bytes);
    AsyncWebServerResponse *response = new (std::nothrow)
        AsyncPreparedResponse(
            content_type,
            bytes->size(),
            [bytes](uint8_t *buffer,
                    size_t max_length,
                    size_t offset) -> size_t {
                if (!buffer || offset >= bytes->size()) return 0;

                const size_t copied = std::min(
                    max_length, bytes->size() - offset);
                memcpy(buffer, bytes->data() + offset, copied);
                return copied;
            });
    if (!response) return false;

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    (void)format_payload_etag(descriptor, etag, sizeof(etag));
    add_artifact_headers(
        response, etag, descriptor.artifact.key.source_revision);
    if (deflated) response->addHeader("Content-Encoding", "deflate");
    request->send(response);
    return true;
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

uint64_t catalog_identity(const NightCatalog &catalog) {
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
    }
    return hash;
}

bool format_catalog_etag(const NightCatalog &catalog,
                         char *out,
                         size_t out_size) {
    if (!out || out_size == 0) return false;

    const int written = snprintf(
        out,
        out_size,
        "\"catalog-%016llx\"",
        static_cast<unsigned long long>(catalog_identity(catalog)));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace

struct ReportHttpController::PendingResponses {
    struct Entry {
        ReportArtifactPayloadDescriptor payload;
        ReportPayloadKind requested_kind = ReportPayloadKind::Whole;
        char series_name[PLOT_SECTION_NAME_BYTES] = {};
        AsyncWebServerRequestPtr request;
        uint32_t deadline_ms = 0;
        bool prefer_deflate = false;

        bool used() const { return payload.valid(); }
        bool needs_section_resolution() const {
            return payload.kind == ReportPayloadKind::PlotIndex &&
                (requested_kind == ReportPayloadKind::PlotEvents ||
                 requested_kind == ReportPayloadKind::PlotSeries);
        }
    };

    StaticSemaphore_t mutex_storage = {};
    SemaphoreHandle_t mutex = nullptr;
    Entry entries[REPORT_HTTP_PENDING_CAPACITY] = {};
};

ReportHttpController::ReportHttpController() = default;
ReportHttpController::~ReportHttpController() = default;

void ReportHttpController::register_routes(AsyncWebServer &server) {
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

void ReportHttpController::begin(ReportTask &report_task) {
    report_task_ = &report_task;

    if (!pending_) {
        pending_.reset(new (std::nothrow) PendingResponses());
    }
    if (pending_ && !pending_->mutex) {
        pending_->mutex =
            xSemaphoreCreateMutexStatic(&pending_->mutex_storage);
    }
}

void ReportHttpController::poll() {
    if (!report_task_ || !pending_ || !pending_->mutex ||
        xSemaphoreTake(pending_->mutex, 0) != pdTRUE) {
        return;
    }

    const uint32_t now_ms = millis();
    for (PendingResponses::Entry &entry : pending_->entries) {
        if (!entry.used()) continue;

        if (entry.request.expired()) {
            entry = {};
            continue;
        }

        ReportArtifactPayloadSelection payload =
            report_task_->select_artifact_payload_if_present(
                entry.payload, entry.prefer_deflate);
        if (payload.state ==
            ReportArtifactPayloadSelectionState::Pending) {
            continue;
        }

        if (payload.ready() && entry.needs_section_resolution()) {
            const ReportArtifactDescriptor &artifact =
                entry.payload.artifact;
            const ReportPlotPayloadQuery section =
                report_task_->query_plot_payload(
                    artifact.key.sleep_day,
                    artifact.key.kind,
                    entry.requested_kind,
                    entry.series_name,
                    artifact.key.range_start_ms,
                    artifact.key.range_end_ms);
            if (section.state == ReportArtifactQueryState::Ready) {
                const OperationAdmission admitted =
                    report_task_->request_payload_cache(
                        section.payload, next_generation());
                if (admitted == OperationAdmission::Accepted) {
                    entry.payload = section.payload;
                    continue;
                }
                if (admitted == OperationAdmission::Busy) continue;
            }

            const AsyncWebServerRequestPtr pending_request = entry.request;
            const ReportArtifactQueryState state = section.state;
            entry = {};
            xSemaphoreGive(pending_->mutex);

            std::shared_ptr<AsyncWebServerRequest> request =
                pending_request.lock();
            if (!request) return;
            if (state == ReportArtifactQueryState::PlotSectionMissing) {
                send_json_error(request.get(), 404, "plot_section_missing");
            } else if (state == ReportArtifactQueryState::ArtifactIndexInvalid) {
                send_json_error(request.get(), 500, "artifact_index_invalid");
            } else {
                send_preparing(request.get());
            }
            return;
        }

        bool superseded = false;
        if (!payload.ready()) {
            const ReportArtifactQuery current = report_task_->query_artifact(
                entry.payload.artifact.key.sleep_day,
                entry.payload.artifact.key.kind,
                entry.payload.artifact.key.range_start_ms,
                entry.payload.artifact.key.range_end_ms);
            superseded = current.state != ReportArtifactQueryState::Ready ||
                !same_artifact_descriptor(
                    current.descriptor, entry.payload.artifact);
        }

        ReportArtifactFailureStatus failure;
        const bool payload_failed = !payload.ready() && !superseded &&
            report_task_->try_payload_failure(entry.payload, failure);
        const bool artifact_failed = !payload.ready() && !superseded &&
            !payload_failed && report_task_->try_artifact_failure(
                entry.payload.artifact.key, failure);
        const bool failed = payload_failed || artifact_failed;
        const bool timed_out =
            millis_deadline_reached(now_ms, entry.deadline_ms);
        if (!payload.ready() && !superseded && !failed && !timed_out) {
            continue;
        }

        const ReportArtifactPayloadDescriptor descriptor = entry.payload;
        const AsyncWebServerRequestPtr pending_request = entry.request;
        entry = {};
        xSemaphoreGive(pending_->mutex);

        std::shared_ptr<AsyncWebServerRequest> request =
            pending_request.lock();
        if (!request) return;

        if (failed) {
            send_artifact_failure(request.get(), failure);
            return;
        }
        if (superseded) {
            send_preparing(request.get());
            return;
        }
        if (!payload.ready()) {
            send_json_error(request.get(), 503, "artifact_payload_timeout");
            return;
        }
        if (!send_artifact_payload(request.get(),
                                   descriptor,
                                   std::move(payload),
                                   "application/octet-stream")) {
            send_json_error(request.get(), 503, "response_alloc");
        }
        return;
    }

    xSemaphoreGive(pending_->mutex);
}

void ReportHttpController::queue_payload_response(
    AsyncWebServerRequest *request,
    const ReportArtifactPayloadDescriptor &payload,
    ReportPayloadKind requested_kind,
    const char *series_name,
    bool prefer_deflate) {
    if (!request || !report_task_ || !pending_ || !pending_->mutex ||
        !payload.valid() ||
        (requested_kind != ReportPayloadKind::Whole &&
         requested_kind != ReportPayloadKind::PlotIndex &&
         requested_kind != ReportPayloadKind::PlotEvents &&
         requested_kind != ReportPayloadKind::PlotSeries)) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }

    ReportArtifactFailureStatus failure;
    const bool failed = payload.is_whole()
        ? report_task_->artifact_failure(payload.artifact.key, failure)
        : report_task_->payload_failure(payload, failure);
    if (failed) {
        send_artifact_failure(request, failure);
        return;
    }

    const OperationAdmission admitted = report_task_->request_payload_cache(
        payload, next_generation());
    if (admitted != OperationAdmission::Accepted) {
        if (admitted == OperationAdmission::Busy) {
            send_preparing(request);
        } else {
            send_json_error(request, 503, "artifact_payload_unavailable");
        }
        return;
    }

    if (xSemaphoreTake(pending_->mutex, 0) != pdTRUE) {
        send_preparing(request);
        return;
    }

    PendingResponses::Entry *free_entry = nullptr;
    for (PendingResponses::Entry &entry : pending_->entries) {
        if (entry.used() && entry.request.expired()) entry = {};
        if (entry.used() && same_payload_descriptor(entry.payload, payload)) {
            xSemaphoreGive(pending_->mutex);
            send_preparing(request);
            return;
        }
        if (!entry.used() && !free_entry) free_entry = &entry;
    }
    if (!free_entry) {
        xSemaphoreGive(pending_->mutex);
        send_preparing(request);
        return;
    }

    free_entry->payload = payload;
    free_entry->requested_kind = requested_kind;
    if (series_name) {
        copy_cstr(free_entry->series_name,
                  sizeof(free_entry->series_name),
                  series_name);
    }
    free_entry->request = request->pause();
    free_entry->deadline_ms = millis() + REPORT_HTTP_PENDING_TIMEOUT_MS;
    free_entry->prefer_deflate = prefer_deflate;
    xSemaphoreGive(pending_->mutex);
}

void ReportHttpController::send_summary(
    AsyncWebServerRequest *request) const {
    if (!report_task_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }
    if (!report_task_available(request, *report_task_)) return;

    const std::shared_ptr<const NightCatalog> catalog =
        report_task_->catalog_snapshot();
    if (!catalog) {
        send_preparing(request);
        return;
    }

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    if (format_catalog_etag(*catalog, etag, sizeof(etag)) &&
        request_etag_matches(request, etag)) {
        send_not_modified(request, etag);
        return;
    }

    std::shared_ptr<LargeTextBuffer> json =
        std::make_shared<LargeTextBuffer>();
    if (!json || !json->reserve(256 + catalog->size() * 256)) {
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

                const size_t remaining = json->length() - offset;
                const size_t copied = remaining < max_length
                    ? remaining
                    : max_length;
                memcpy(buffer, json->c_str() + offset, copied);
                return copied;
            });
    if (!response) {
        send_json_error(request, 503, "response_alloc");
        return;
    }

    add_artifact_headers(response, etag);
    request->send(response);
}

void ReportHttpController::send_result(
    AsyncWebServerRequest *request) {
    if (!report_task_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }

    SleepDayId sleep_day;
    if (!parse_sleep_day(request, sleep_day)) {
        send_json_error(request, 400, "bad_night");
        return;
    }

    send_artifact(request, sleep_day, ReportArtifactKind::Result);
}

void ReportHttpController::send_plot(
    AsyncWebServerRequest *request) {
    if (!report_task_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }

    SleepDayId sleep_day;
    if (!parse_sleep_day(request, sleep_day)) {
        send_json_error(request, 400, "bad_night");
        return;
    }

    ReportArtifactKind kind = ReportArtifactKind::Overview;
    int64_t range_start_ms = 0;
    int64_t range_end_ms = 0;
    const bool range_requested =
        request->hasArg("from") || request->hasArg("to");
    if (range_requested) {
        if (!parse_positive_int64(request, "from", range_start_ms) ||
            !parse_positive_int64(request, "to", range_end_ms)) {
            send_json_error(request, 400, "bad_range");
            return;
        }
        kind = ReportArtifactKind::RangeTile;
    }

    if (!request->hasArg("part")) {
        send_artifact(
            request, sleep_day, kind, range_start_ms, range_end_ms);
        return;
    }

    const String part = request->arg("part");
    ReportPayloadKind payload_kind = ReportPayloadKind::PlotIndex;
    String series_name;
    if (part == "events") {
        payload_kind = ReportPayloadKind::PlotEvents;
    } else if (part == "series") {
        payload_kind = ReportPayloadKind::PlotSeries;
        if (!request->hasArg("name")) {
            send_json_error(request, 400, "missing_series_name");
            return;
        }
        series_name = request->arg("name");
        if (!series_name.length() ||
            series_name.length() >= PLOT_SECTION_NAME_BYTES) {
            send_json_error(request, 400, "bad_series_name");
            return;
        }
    } else if (part != "index") {
        send_json_error(request, 400, "bad_plot_part");
        return;
    }

    const ReportPlotPayloadQuery query = report_task_->query_plot_payload(
        sleep_day,
        kind,
        payload_kind,
        series_name.length() ? series_name.c_str() : nullptr,
        range_start_ms,
        range_end_ms);
    if (query.state == ReportArtifactQueryState::Unavailable) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }
    if (query.state == ReportArtifactQueryState::CatalogPending) {
        send_preparing(request);
        return;
    }
    if (query.state == ReportArtifactQueryState::NightMissing) {
        send_json_error(request, 404, "no_such_night");
        return;
    }
    if (query.state == ReportArtifactQueryState::InvalidArtifact) {
        send_json_error(
            request,
            kind == ReportArtifactKind::RangeTile ? 400 : 500,
            kind == ReportArtifactKind::RangeTile
                ? "range_not_one_tile"
                : "artifact_index_invalid");
        return;
    }
    if (query.state == ReportArtifactQueryState::ArtifactMissing) {
        const OperationAdmission admitted = report_task_->request_artifact(
            query.artifact,
            ReportRequestPriority::Foreground,
            next_generation());
        if (admitted == OperationAdmission::Accepted) {
            send_preparing(request);
        } else {
            send_json_error(request, 503, "report_queue_busy");
        }
        return;
    }
    if (query.state == ReportArtifactQueryState::PlotSectionMissing) {
        send_json_error(request, 404, "plot_section_missing");
        return;
    }
    if (query.state == ReportArtifactQueryState::ArtifactIndexInvalid) {
        send_json_error(request, 500, "artifact_index_invalid");
        return;
    }
    if (query.state != ReportArtifactQueryState::Ready &&
        query.state != ReportArtifactQueryState::PlotIndexPending) {
        send_json_error(request, 500, "artifact_query_invalid");
        return;
    }

    ReportArtifactFailureStatus payload_failure;
    if (report_task_->payload_failure(query.payload, payload_failure)) {
        send_artifact_failure(request, payload_failure);
        return;
    }

    if (query.state == ReportArtifactQueryState::Ready) {
        char etag[REPORT_HTTP_ETAG_BYTES] = {};
        if (format_payload_etag(query.payload, etag, sizeof(etag)) &&
            request_etag_matches(request, etag)) {
            send_not_modified(
                request,
                etag,
                query.payload.artifact.key.source_revision);
            return;
        }
    }

    const bool prefer_deflate = request_accepts_deflate(request) &&
        query.payload.size >= AC_REPORT_HTTP_DEFLATE_MIN_BYTES;
    if (query.state == ReportArtifactQueryState::Ready) {
        ReportArtifactPayloadSelection payload =
            report_task_->select_artifact_payload(
                query.payload, prefer_deflate);
        if (payload.ready() && send_artifact_payload(
                                   request,
                                   query.payload,
                                   std::move(payload),
                                   "application/octet-stream")) {
            return;
        }
    }

    queue_payload_response(
        request,
        query.payload,
        payload_kind,
        series_name.length() ? series_name.c_str() : nullptr,
        prefer_deflate);
}

void ReportHttpController::send_artifact(
    AsyncWebServerRequest *request,
    SleepDayId sleep_day,
    ReportArtifactKind kind,
    int64_t range_start_ms,
    int64_t range_end_ms) {
    const ReportArtifactQuery query = report_task_->query_artifact(
        sleep_day, kind, range_start_ms, range_end_ms);
    if (query.state == ReportArtifactQueryState::Unavailable) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }
    if (query.state == ReportArtifactQueryState::CatalogPending) {
        send_preparing(request);
        return;
    }
    if (query.state == ReportArtifactQueryState::NightMissing) {
        send_json_error(request, 404, "no_such_night");
        return;
    }
    if (query.state == ReportArtifactQueryState::InvalidArtifact) {
        send_json_error(
            request,
            kind == ReportArtifactKind::RangeTile ? 400 : 500,
            kind == ReportArtifactKind::RangeTile
                ? "range_not_one_tile"
                : "artifact_index_invalid");
        return;
    }
    if (query.state == ReportArtifactQueryState::ArtifactIndexInvalid) {
        send_json_error(request, 500, "artifact_index_invalid");
        return;
    }
    if (query.state == ReportArtifactQueryState::ArtifactMissing) {
        ReportArtifactFailureStatus failure;
        if (report_task_->artifact_failure(query.artifact, failure)) {
            send_artifact_failure(request, failure);
            return;
        }

        const OperationAdmission admitted = report_task_->request_artifact(
            query.artifact,
            ReportRequestPriority::Foreground,
            next_generation());
        if (admitted == OperationAdmission::Accepted) {
            send_preparing(request);
        } else {
            send_json_error(request, 503, "report_queue_busy");
        }
        return;
    }
    if (query.state != ReportArtifactQueryState::Ready) {
        send_json_error(request, 500, "artifact_query_invalid");
        return;
    }

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    if (format_artifact_etag(query.descriptor, etag, sizeof(etag)) &&
        request_etag_matches(request, etag)) {
        send_not_modified(
            request, etag, query.artifact.source_revision);
        return;
    }

    const bool prefer_deflate = request_accepts_deflate(request) &&
        query.descriptor.size >= AC_REPORT_HTTP_DEFLATE_MIN_BYTES;
    ReportArtifactPayloadSelection payload =
        report_task_->select_artifact_payload(
            query.descriptor, prefer_deflate);
    if (payload.ready() && send_artifact_payload(
                               request,
                               ReportArtifactPayloadDescriptor::whole(
                                   query.descriptor),
                               std::move(payload),
                               "application/octet-stream")) {
        return;
    }

    queue_payload_response(
        request,
        ReportArtifactPayloadDescriptor::whole(query.descriptor),
        ReportPayloadKind::Whole,
        nullptr,
        prefer_deflate);
}

uint32_t ReportHttpController::next_generation() const {
    uint32_t generation = next_generation_.fetch_add(
        1, std::memory_order_relaxed);
    if (generation != 0) return generation;

    generation = next_generation_.fetch_add(1, std::memory_order_relaxed);
    return generation == 0 ? 1 : generation;
}

}  // namespace aircannect
