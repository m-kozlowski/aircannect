#include "report_http_controller.h"

#include <ESPAsyncWebServer.h>

#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "async_prepared_response.h"
#include "json_util.h"
#include "large_text_buffer.h"
#include "night_catalog.h"
#include "report_artifacts.h"
#include "report_range_tile.h"
#include "report_task.h"
#include "storage_stream_port.h"

namespace aircannect {
namespace {

static constexpr size_t REPORT_HTTP_ETAG_BYTES = 112;

class ReportHttpStream {
public:
    ReportHttpStream(StorageStreamPort &port,
                     std::shared_ptr<StorageByteStream> stream,
                     size_t expected_size) :
        port_(port), stream_(std::move(stream)), expected_size_(expected_size) {}

    ~ReportHttpStream() {
        if (stream_) port_.finish(*stream_, complete_);
    }

    size_t fill(uint8_t *buffer, size_t max_length, size_t offset) {
        if (!stream_ || !buffer || max_length == 0 ||
            offset > expected_size_) {
            return 0;
        }

        if (!attached_) {
            StorageStreamStatus status;
            if (!port_.status(*stream_, status)) return RESPONSE_TRY_AGAIN;
            if (status.state == StorageStreamState::Preparing) {
                return RESPONSE_TRY_AGAIN;
            }
            if (status.state != StorageStreamState::Ready ||
                status.size != expected_size_) {
                return 0;
            }
            if (!port_.attach(*stream_)) return RESPONSE_TRY_AGAIN;
            attached_ = true;
        }

        const StorageStreamRead read = port_.read(
            *stream_, buffer, max_length, offset);
        if (read.state == StorageStreamReadState::Retry) {
            return RESPONSE_TRY_AGAIN;
        }
        if (read.state != StorageStreamReadState::Data || read.bytes == 0 ||
            read.bytes > expected_size_ - offset) {
            return 0;
        }

        complete_ = offset + read.bytes == expected_size_;
        return read.bytes;
    }

private:
    StorageStreamPort &port_;
    std::shared_ptr<StorageByteStream> stream_;
    size_t expected_size_ = 0;
    bool attached_ = false;
    bool complete_ = false;
};

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
            "\"%c-%s-%016llx-%lld-%llu-%08lx\"",
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
            "\"%c-%s-%016llx-%llu-%08lx\"",
            kind,
            day,
            static_cast<unsigned long long>(
                artifact.key.source_revision.value()),
            static_cast<unsigned long long>(artifact.size),
            static_cast<unsigned long>(artifact.crc32));
    }
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
                          const char *etag) {
    if (!response) return;

    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Accept-Ranges", "none");
    if (etag && etag[0]) response->addHeader("ETag", etag);
}

void send_not_modified(AsyncWebServerRequest *request, const char *etag) {
    AsyncWebServerResponse *response = request->beginResponse(304);
    if (!response) {
        request->send(304);
        return;
    }

    add_artifact_headers(response, etag);
    request->send(response);
}

bool send_artifact_stream(AsyncWebServerRequest *request,
                          StorageStreamPort &port,
                          const ReportArtifactDescriptor &artifact,
                          const char *content_type) {
    if (!artifact.valid() || artifact.size > SIZE_MAX) {
        send_json_error(request, 500, "artifact_invalid");
        return false;
    }

    char path[AC_STORAGE_PATH_MAX] = {};
    if (!artifact.path(path, sizeof(path))) {
        send_json_error(request, 500, "artifact_path_invalid");
        return false;
    }

    StorageStreamCommand command;
    command.path = path;
    command.lane = StorageStreamLane::Foreground;
    command.expected_size = artifact.size;
    command.verification = StorageStreamVerification::Size;

    std::shared_ptr<StorageByteStream> byte_stream;
    char stream_error[AC_STORAGE_ERROR_MAX] = {};
    if (!port.request_stream(command,
                             byte_stream,
                             stream_error,
                             sizeof(stream_error))) {
        send_json_error(request,
                        503,
                        stream_error[0] ? stream_error
                                        : "artifact_stream_unavailable");
        return false;
    }

    std::shared_ptr<ReportHttpStream> stream(
        new (std::nothrow) ReportHttpStream(
            port, byte_stream, static_cast<size_t>(artifact.size)));
    if (!stream) {
        port.finish(*byte_stream, false);
        send_json_error(request, 503, "response_alloc");
        return false;
    }

    AsyncWebServerResponse *response = new (std::nothrow)
        AsyncPreparedResponse(
            content_type,
            static_cast<size_t>(artifact.size),
            [stream](uint8_t *buffer,
                     size_t max_length,
                     size_t offset) -> size_t {
                return stream->fill(buffer, max_length, offset);
            });
    if (!response) {
        send_json_error(request, 503, "response_alloc");
        return false;
    }

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    (void)format_artifact_etag(artifact, etag, sizeof(etag));
    add_artifact_headers(response, etag);
    request->send(response);
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

uint32_t night_duration_minutes(const NightCatalog &catalog,
                                const NightCatalogRecord &night) {
    if (night.metrics.has(NightCatalogMetric::DurationMinutes)) {
        return night.metrics.duration_min;
    }

    uint64_t duration_ms = 0;
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, session_count);
    for (size_t i = 0; sessions && i < session_count; ++i) {
        const NightCatalogTimeRange &session = sessions[i];
        if (!session.valid()) continue;
        duration_ms += static_cast<uint64_t>(
            session.end_ms - session.start_ms);
    }
    return static_cast<uint32_t>(duration_ms / 60000);
}

}  // namespace

void ReportHttpController::begin(ReportTask &report_task,
                                 StorageStreamPort &stream_port) {
    report_task_ = &report_task;
    stream_port_ = &stream_port;
}

void ReportHttpController::send_summary(
    AsyncWebServerRequest *request) const {
    if (!report_task_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }

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

    const ReportTaskStatus status = report_task_->status();
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
                     night_duration_minutes(*catalog, *night)));
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
    AsyncWebServerRequest *request) const {
    if (!report_task_ || !stream_port_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }

    SleepDayId sleep_day;
    if (!parse_sleep_day(request, sleep_day)) {
        send_json_error(request, 400, "bad_night");
        return;
    }

    const std::shared_ptr<const NightCatalog> catalog =
        report_task_->catalog_snapshot();
    if (!catalog) {
        send_preparing(request);
        return;
    }

    const NightCatalogRecord *night = catalog->find(sleep_day);
    if (!night) {
        send_json_error(request, 404, "no_such_night");
        return;
    }

    const ReportArtifactKey key = ReportArtifactKey::result(
        sleep_day, night->source_revision);
    ReportArtifactAvailability availability;
    if (!report_task_->artifact_availability(key, availability)) {
        const OperationAdmission admitted = report_task_->request_artifact(
            key, ReportRequestPriority::Foreground, next_generation());
        if (admitted == OperationAdmission::Accepted) {
            send_preparing(request);
        } else {
            send_json_error(request, 503, "report_queue_busy");
        }
        return;
    }

    ReportArtifactDescriptor artifact;
    if (!availability.descriptor(key, artifact)) {
        send_json_error(request, 500, "artifact_index_invalid");
        return;
    }

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    if (format_artifact_etag(artifact, etag, sizeof(etag)) &&
        request_etag_matches(request, etag)) {
        send_not_modified(request, etag);
        return;
    }

    (void)send_artifact_stream(
        request, *stream_port_, artifact, "application/octet-stream");
}

void ReportHttpController::send_plot(
    AsyncWebServerRequest *request) const {
    if (!report_task_ || !stream_port_) {
        send_json_error(request, 503, "report_unavailable");
        return;
    }

    SleepDayId sleep_day;
    if (!parse_sleep_day(request, sleep_day)) {
        send_json_error(request, 400, "bad_night");
        return;
    }

    const std::shared_ptr<const NightCatalog> catalog =
        report_task_->catalog_snapshot();
    if (!catalog) {
        send_preparing(request);
        return;
    }

    const NightCatalogRecord *night = catalog->find(sleep_day);
    if (!night) {
        send_json_error(request, 404, "no_such_night");
        return;
    }

    ReportArtifactKey key = ReportArtifactKey::overview(
        sleep_day, night->source_revision);
    const bool range_requested =
        request->hasArg("from") || request->hasArg("to");
    if (range_requested) {
        int64_t from_ms = 0;
        int64_t to_ms = 0;
        if (!parse_positive_int64(request, "from", from_ms) ||
            !parse_positive_int64(request, "to", to_ms)) {
            send_json_error(request, 400, "bad_range");
            return;
        }

        key = ReportArtifactKey::range_tile(
            sleep_day, night->source_revision, from_ms, to_ms);
        if (!key.valid()) {
            send_json_error(request, 400, "range_not_one_tile");
            return;
        }
    }

    ReportArtifactAvailability availability;
    if (!report_task_->artifact_availability(key, availability)) {
        const OperationAdmission admitted = report_task_->request_artifact(
            key, ReportRequestPriority::Foreground, next_generation());
        if (admitted == OperationAdmission::Accepted) {
            send_preparing(request);
        } else {
            send_json_error(request, 503, "report_queue_busy");
        }
        return;
    }

    ReportArtifactDescriptor artifact;
    if (!availability.descriptor(key, artifact)) {
        send_json_error(request, 500, "artifact_index_invalid");
        return;
    }

    char etag[REPORT_HTTP_ETAG_BYTES] = {};
    if (format_artifact_etag(artifact, etag, sizeof(etag)) &&
        request_etag_matches(request, etag)) {
        send_not_modified(request, etag);
        return;
    }

    (void)send_artifact_stream(
        request, *stream_port_, artifact, "application/octet-stream");
}

uint32_t ReportHttpController::next_generation() const {
    uint32_t generation = next_generation_.fetch_add(
        1, std::memory_order_relaxed);
    if (generation != 0) return generation;

    generation = next_generation_.fetch_add(1, std::memory_order_relaxed);
    return generation == 0 ? 1 : generation;
}

}  // namespace aircannect
