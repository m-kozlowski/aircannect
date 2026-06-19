#include "spool_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "board_report.h"

namespace aircannect {
namespace {

std::string sha_to_hex(const uint8_t hash[32]) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = digits[(hash[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[hash[i] & 0x0F];
    }
    return out;
}

bool equal_ci(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 32);
        if (ca != cb) return false;
    }
    return true;
}

bool json_get_error(JsonDocument &doc, std::string &out) {
    if (!doc["error"].is<JsonObject>()) return false;
    JsonObject error = doc["error"].as<JsonObject>();
    const char *message = error["message"] | "rpc_error";
    out = message ? message : "rpc_error";
    return true;
}

struct JsonStringView {
    const char *data = nullptr;
    size_t len = 0;
};

const char *skip_json_ws(const char *p, const char *end) {
    while (p < end &&
           (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

const char *find_json_key(const std::string &json, const char *key) {
    std::string needle;
    needle.reserve(strlen(key) + 2);
    needle += '"';
    needle += key;
    needle += '"';
    size_t pos = json.find(needle);
    while (pos != std::string::npos) {
        const char *p = json.c_str() + pos + needle.size();
        const char *end = json.c_str() + json.size();
        p = skip_json_ws(p, end);
        if (p < end && *p == ':') return p + 1;
        pos = json.find(needle, pos + 1);
    }
    return nullptr;
}

bool json_string_view(const std::string &json,
                      const char *key,
                      JsonStringView &out) {
    const char *p = find_json_key(json, key);
    if (!p) return false;
    const char *end = json.c_str() + json.size();
    p = skip_json_ws(p, end);
    if (p >= end || *p != '"') return false;
    p++;
    const char *start = p;
    bool escaped = false;
    while (p < end) {
        if (escaped) {
            escaped = false;
        } else if (*p == '\\') {
            escaped = true;
        } else if (*p == '"') {
            out.data = start;
            out.len = static_cast<size_t>(p - start);
            return true;
        }
        p++;
    }
    return false;
}

bool json_uint_value(const std::string &json,
                     const char *key,
                     uint32_t &out) {
    const char *p = find_json_key(json, key);
    if (!p) return false;
    const char *end = json.c_str() + json.size();
    p = skip_json_ws(p, end);
    if (p >= end || *p < '0' || *p > '9') return false;
    uint32_t value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        p++;
    }
    out = value;
    return true;
}

bool json_object_text(const std::string &json,
                      const char *key,
                      std::string &out) {
    const char *p = find_json_key(json, key);
    if (!p) return false;
    const char *end = json.c_str() + json.size();
    p = skip_json_ws(p, end);
    if (p >= end || *p != '{') return false;

    const char *start = p;
    uint16_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    while (p < end) {
        const char ch = *p;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            if (!depth) return false;
            depth--;
            if (!depth) {
                out.assign(start, static_cast<size_t>(p - start + 1));
                return true;
            }
        }
        p++;
    }
    return false;
}

bool json_string_equals(const JsonStringView &view, const char *value) {
    const size_t len = strlen(value);
    return view.len == len && view.data && memcmp(view.data, value, len) == 0;
}

}  // namespace

const char *spool_client_state_name(SpoolClientState state) {
    switch (state) {
        case SpoolClientState::Idle: return "idle";
        case SpoolClientState::Starting: return "starting";
        case SpoolClientState::Pulling: return "pulling";
        case SpoolClientState::WaitingFragments: return "fragments";
        case SpoolClientState::Complete: return "complete";
        case SpoolClientState::Error: return "error";
    }
    return "unknown";
}

SpoolClient::~SpoolClient() {
    clear_round_fragments(true);
}

bool SpoolClient::begin(const SpoolClientRequest &request) {
    reset();
    if (request.spool_type.empty() || request.from_dt.empty()) return false;
    request_ = request;
    if (request_.max_size == 0) request_.max_size = 65536;
    if (request_.fragment_max == 0) request_.fragment_max = 2808;
    if (request_.max_rounds == 0) request_.max_rounds = 64;
    result_.spool_type = request_.spool_type;
    result_.from_dt = request_.from_dt;
    result_.payload.set_max_size(AC_REPORT_MAX_PAYLOAD_BYTES);
    status_.spool_type = request_.spool_type;
    fetch_started_ms_ = millis();
    schedule_start();
    return true;
}

void SpoolClient::reset() {
    request_ = {};
    status_ = {};
    result_.clear();
    state_ = State::Idle;
    pending_submit_ = PendingSubmit::None;
    pending_id_ = 0;
    active_spool_id_ = 0;
    state_started_ms_ = 0;
    fetch_started_ms_ = 0;
    next_pull_submit_ms_ = 0;
    next_spool_address_json_.clear();
    clear_round_fragments(true);
    round_start_offset_ = 0;
    round_start_fragment_count_ = 0;
    round_retry_count_ = 0;
    completed_round_ready_ = false;
    completed_round_.clear();
}

bool SpoolClient::active() const {
    return state_ != State::Idle && state_ != State::Done &&
           state_ != State::Failed;
}

void SpoolClient::poll(RpcArbiter &arbiter) {
    const uint32_t now = millis();
    update_status(now);

    switch (state_) {
        case State::Idle:
        case State::Done:
        case State::Failed:
            return;
        case State::WaitStart:
        case State::WaitPull:
        case State::WaitFragments:
        case State::RoundReady:
            break;
    }

    if (state_ == State::RoundReady) {
        return;
    }

    if (pending_submit_ != PendingSubmit::None) {
        if (request_.pace_on_backpressure &&
            arbiter.background_backpressure_active()) {
            return;
        }
        if (request_.pace_on_backpressure &&
            pending_submit_ == PendingSubmit::Pull &&
            next_pull_submit_ms_ != 0 &&
            static_cast<int32_t>(now - next_pull_submit_ms_) < 0) {
            return;
        }
        submit_pending(arbiter);
        return;
    }

    switch (state_) {
        case State::Idle:
        case State::Done:
        case State::Failed:
            return;
        case State::WaitStart:
            if (now - state_started_ms_ > AC_SPOOL_CLIENT_RPC_TIMEOUT_MS) {
                fail("rpc_timeout");
            }
            return;
        case State::WaitPull:
            if (now - state_started_ms_ >
                AC_SPOOL_CLIENT_PULL_RPC_TIMEOUT_MS) {
                fail("rpc_timeout");
            }
            return;
        case State::WaitFragments:
            if (now - state_started_ms_ >
                AC_SPOOL_CLIENT_FRAGMENT_TIMEOUT_MS) {
                retry_current_round("fragment_timeout");
            }
            return;
        case State::RoundReady:
            return;
    }
}

void SpoolClient::schedule_start() {
    status_.state = SpoolClientState::Starting;
    status_.current_round = result_.rounds + 1;
    status_.fragments = result_.fragments;
    status_.bytes = result_.bytes;
    pending_id_ = 0;
    pending_submit_ = PendingSubmit::Start;
    state_ = State::WaitStart;
    state_started_ms_ = millis();
}

void SpoolClient::schedule_pull() {
    pending_id_ = 0;
    pending_submit_ = PendingSubmit::Pull;
    state_ = State::WaitPull;
    status_.state = SpoolClientState::Pulling;
    state_started_ms_ = millis();
}

bool SpoolClient::submit_pending(RpcArbiter &arbiter) {
    switch (pending_submit_) {
        case PendingSubmit::None:
            return true;
        case PendingSubmit::Start:
            return submit_start(arbiter);
        case PendingSubmit::Pull:
            return submit_pull(arbiter);
    }
    return false;
}

bool SpoolClient::submit_start(RpcArbiter &arbiter) {
    uint32_t id = 0;
    if (!arbiter.send_request_with_id("StartSpool", build_start_params(),
                                      RpcSource::Report,
                                      AC_SPOOL_CLIENT_RPC_TIMEOUT_MS, id)) {
        fail("submit_start_failed");
        return false;
    }
    pending_id_ = id;
    pending_submit_ = PendingSubmit::None;
    state_started_ms_ = millis();
    return true;
}

bool SpoolClient::submit_pull(RpcArbiter &arbiter) {
    uint32_t id = 0;
    if (!arbiter.send_request_with_id("PullSpoolFragments",
                                      build_pull_params(),
                                      RpcSource::Report,
                                      AC_SPOOL_CLIENT_PULL_RPC_TIMEOUT_MS,
                                      id)) {
        fail("submit_pull_failed");
        return false;
    }
    pending_id_ = id;
    pending_submit_ = PendingSubmit::None;
    state_started_ms_ = millis();
    if (request_.pace_on_backpressure) {
        next_pull_submit_ms_ = state_started_ms_ +
            AC_REPORT_SPOOL_PULL_PACE_MS;
        if (next_pull_submit_ms_ == 0) next_pull_submit_ms_ = 1;
    }
    return true;
}

bool SpoolClient::handle_event(const RpcEvent &event) {
    if (!active()) return false;
    if (event.kind == RpcEventKind::RpcResponse && pending_id_ &&
        event.id == pending_id_) {
        if (state_ == State::WaitStart) {
            handle_start_response(event.payload_text());
        } else if (state_ == State::WaitPull) {
            handle_pull_response(event.payload_text());
        }
        return true;
    }
    if (event.kind == RpcEventKind::RpcNotification &&
        event.source == RpcSource::Report &&
        (state_ == State::WaitPull || state_ == State::WaitFragments)) {
        return handle_spool_fragment(event);
    }
    return false;
}

bool SpoolClient::handle_start_response(const std::string &payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload.c_str())) {
        fail("bad_start_response");
        return false;
    }
    std::string error;
    if (json_get_error(doc, error)) {
        fail(error.c_str());
        return false;
    }
    active_spool_id_ = doc["result"]["spoolId"] | 0;
    if (!active_spool_id_) {
        fail("zero_spool_id");
        return false;
    }
    result_.rounds++;
    status_.current_round = result_.rounds;
    round_start_offset_ = result_.payload.size();
    round_start_fragment_count_ = result_.fragments;
    clear_round_fragments(true);
    schedule_pull();
    return true;
}

bool SpoolClient::handle_pull_response(const std::string &payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload.c_str())) {
        fail("bad_pull_response");
        return false;
    }
    std::string error;
    if (json_get_error(doc, error)) {
        fail(error.c_str());
        return false;
    }
    state_ = State::WaitFragments;
    state_started_ms_ = millis();
    status_.state = SpoolClientState::WaitingFragments;
    return true;
}

bool SpoolClient::handle_spool_fragment(const RpcEvent &event) {
    const std::string &payload = event.payload_text();
    JsonStringView method;
    if (!json_string_view(payload, "method", method) ||
        !json_string_equals(method, "SpoolFragment")) {
        return false;
    }

    uint32_t spool_id = 0;
    if (!json_uint_value(payload, "spoolId", spool_id)) return false;
    if (!active_spool_id_ || spool_id != active_spool_id_) return false;

    uint32_t seq = result_.fragments;
    json_uint_value(payload, "seq", seq);

    JsonStringView data;
    if (!json_string_view(payload, "data", data) ||
        !append_base64_fragment(data.data, data.len, seq)) {
        retry_current_round("fragment_decode_failed");
        return true;
    }

    result_.fragments++;
    if (!request_.stream_rounds) {
        result_.bytes = result_.payload.size();
        status_.bytes = result_.bytes;
    } else {
        status_.bytes = result_.bytes +
            static_cast<uint32_t>(result_.payload.size() -
                                  round_start_offset_);
    }
    status_.fragments = result_.fragments;
    status_.round_fragments = static_cast<uint32_t>(round_fragment_count_);
    state_started_ms_ = millis();

    JsonStringView status_view;
    if (!json_string_view(payload, "status", status_view) ||
        json_string_equals(status_view, "SPOOL_INCOMPLETE")) {
        if (request_.max_notifications > 0) {
            schedule_pull();
        }
        return true;
    }

    result_.terminal_status.assign(status_view.data, status_view.len);
    JsonStringView hash_view;
    if (json_string_view(payload, "spoolHash", hash_view)) {
        result_.spool_hash.assign(hash_view.data, hash_view.len);
    } else {
        result_.spool_hash.clear();
    }
    std::string pending_next_spool_address;
    json_object_text(payload, "nextSpoolAddress", pending_next_spool_address);

    if (!normalize_current_round()) {
        retry_current_round("fragment_sequence");
        return true;
    }
    if (request_.stream_rounds) {
        status_.bytes = result_.bytes +
            static_cast<uint32_t>(result_.payload.size() -
                                  round_start_offset_);
    } else {
        result_.bytes = result_.payload.size();
        status_.bytes = result_.bytes;
    }
    if (result_.spool_hash.empty()) {
        retry_current_round("missing_spool_hash");
        return true;
    }
    if (!compute_hash(round_start_offset_,
                      result_.payload.size() - round_start_offset_)) {
        Log::logf(CAT_RPC, LOG_WARN,
                  "[SPOOL] sha256 mismatch spool=%s round=%lu "
                  "fragments=%u bytes=%lu expected=%s actual=%s\n",
                  result_.spool_type.c_str(),
                  static_cast<unsigned long>(result_.rounds),
                  static_cast<unsigned>(round_fragment_count_),
                  static_cast<unsigned long>(
                      result_.payload.size() - round_start_offset_),
                  result_.spool_hash.c_str(),
                  result_.computed_sha256.c_str());
        retry_current_round("sha256_mismatch");
        return true;
    }

    next_spool_address_json_ = pending_next_spool_address;
    round_retry_count_ = 0;
    if (request_.stream_rounds) {
        if (!capture_completed_round()) {
            retry_current_round("round_capture_failed");
            return true;
        }
        clear_round_fragments(true);
        state_ = State::RoundReady;
        status_.state = SpoolClientState::WaitingFragments;
        state_started_ms_ = millis();
        return true;
    }

    clear_round_fragments(true);
    if (result_.terminal_status == "SPOOL_COMPLETE_MORE_DATA_PENDING" &&
        result_.rounds < request_.max_rounds &&
        !next_spool_address_json_.empty()) {
        schedule_start();
        return true;
    }

    if (result_.terminal_status == "SPOOL_COMPLETE_MORE_DATA_PENDING") {
        result_.truncated = true;
    }
    finish();
    return true;
}

bool SpoolClient::append_base64_fragment(const char *data,
                                         size_t input_len,
                                         uint32_t seq) {
    if (!ensure_round_fragment_capacity(round_fragment_count_ + 1)) {
        return false;
    }
    FragmentSlice &fragment = round_fragments_[round_fragment_count_];
    fragment.seq = seq;
    fragment.offset = result_.payload.size();
    fragment.len = 0;
    if (!data) return false;
    if (!input_len) {
        round_fragment_count_++;
        return true;
    }
    size_t needed = 0;
    int rc = mbedtls_base64_decode(nullptr, 0, &needed,
                                   reinterpret_cast<const unsigned char *>(
                                       data),
                                   input_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return false;
    const size_t offset = result_.payload.size();
    if (!needed) {
        round_fragment_count_++;
        return true;
    }
    size_t tail_offset = 0;
    uint8_t *decoded = result_.payload.append_uninitialized(needed,
                                                            tail_offset);
    if (!decoded && needed) return false;
    if (tail_offset != offset) {
        result_.payload.truncate(offset);
        return false;
    }
    size_t written = 0;
    rc = mbedtls_base64_decode(decoded, needed, &written,
                               reinterpret_cast<const unsigned char *>(data),
                               input_len);
    bool ok = rc == 0;
    result_.payload.truncate(ok ? offset + written : offset);
    if (ok) {
        fragment.offset = offset;
        fragment.len = written;
        round_fragment_count_++;
    }
    return ok;
}

bool SpoolClient::ensure_round_fragment_capacity(size_t count) {
    if (count <= round_fragment_capacity_) return true;
    size_t next_capacity = round_fragment_capacity_ ? round_fragment_capacity_ : 32;
    while (next_capacity < count) next_capacity *= 2;
    void *memory = Memory::alloc_large(next_capacity * sizeof(FragmentSlice));
    if (!memory) return false;
    FragmentSlice *next = static_cast<FragmentSlice *>(memory);
    if (round_fragments_ && round_fragment_count_) {
        memcpy(next,
               round_fragments_,
               round_fragment_count_ * sizeof(FragmentSlice));
    }
    Memory::free(round_fragments_);
    round_fragments_ = next;
    round_fragment_capacity_ = next_capacity;
    return true;
}

void SpoolClient::clear_round_fragments(bool release_storage) {
    round_fragment_count_ = 0;
    if (release_storage) {
        Memory::free(round_fragments_);
        round_fragments_ = nullptr;
        round_fragment_capacity_ = 0;
    }
}

bool SpoolClient::normalize_current_round() {
    if (!round_fragment_count_) return true;
    std::stable_sort(round_fragments_,
                     round_fragments_ + round_fragment_count_,
                     [](const FragmentSlice &a, const FragmentSlice &b) {
                         return a.seq < b.seq;
                     });

    bool already_ordered = true;
    for (size_t i = 0; i < round_fragment_count_; ++i) {
        const FragmentSlice &fragment = round_fragments_[i];
        if (fragment.seq != i) {
            const bool duplicate = fragment.seq < i;
            Log::logf(CAT_RPC, LOG_WARN,
                      "[SPOOL] fragment sequence %s spool=%s round=%lu "
                      "index=%u expected=%u got=%lu fragments=%u\n",
                      duplicate ? "duplicate" : "gap",
                      result_.spool_type.c_str(),
                      static_cast<unsigned long>(result_.rounds),
                      static_cast<unsigned>(i),
                      static_cast<unsigned>(i),
                      static_cast<unsigned long>(fragment.seq),
                      static_cast<unsigned>(round_fragment_count_));
            return false;
        }
        if (i == 0) {
            if (fragment.offset != round_start_offset_) {
                already_ordered = false;
            }
        } else {
            const FragmentSlice &previous = round_fragments_[i - 1];
            const size_t expected = previous.offset + previous.len;
            if (fragment.offset != expected) {
                already_ordered = false;
            }
        }
    }
    if (already_ordered) return true;

    ReportSpoolBuffer ordered;
    ordered.set_max_size(AC_REPORT_MAX_PAYLOAD_BYTES);
    if (round_start_offset_ &&
        !ordered.append(result_.payload.data(), round_start_offset_)) {
        return false;
    }
    for (size_t i = 0; i < round_fragment_count_; ++i) {
        const FragmentSlice &fragment = round_fragments_[i];
        if (fragment.offset > result_.payload.size() ||
            fragment.len > result_.payload.size() - fragment.offset) {
            return false;
        }
        if (fragment.len &&
            !ordered.append(result_.payload.data() + fragment.offset,
                            fragment.len)) {
            return false;
        }
    }
    result_.payload.move_from(ordered);
    return true;
}

bool SpoolClient::compute_hash(size_t offset, size_t len) {
    if (result_.spool_hash.empty()) {
        result_.sha_ok = false;
        result_.complete = false;
        result_.computed_sha256.clear();
        return false;
    }
    if (offset > result_.payload.size() ||
        len > result_.payload.size() - offset) {
        return false;
    }
    uint8_t hash[32] = {};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    if (len) {
        mbedtls_sha256_update(&ctx, result_.payload.data() + offset, len);
    }
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    result_.computed_sha256 = sha_to_hex(hash);
    result_.sha_ok = equal_ci(result_.computed_sha256, result_.spool_hash);
    result_.complete = result_.sha_ok;
    return result_.sha_ok;
}

bool SpoolClient::capture_completed_round() {
    if (completed_round_ready_) return false;
    if (round_start_offset_ > result_.payload.size()) return false;
    const size_t round_len = result_.payload.size() - round_start_offset_;
    completed_round_.clear();
    completed_round_.spool_type = result_.spool_type;
    completed_round_.from_dt = result_.from_dt;
    completed_round_.terminal_status = result_.terminal_status;
    completed_round_.spool_hash = result_.spool_hash;
    completed_round_.computed_sha256 = result_.computed_sha256;
    completed_round_.sha_ok = result_.sha_ok;
    completed_round_.complete = true;
    completed_round_.rounds = 1;
    completed_round_.fragments =
        static_cast<uint32_t>(round_fragment_count_);
    completed_round_.bytes = static_cast<uint32_t>(round_len);
    completed_round_.payload.set_max_size(AC_REPORT_MAX_PAYLOAD_BYTES);
    if (round_len) {
        if (round_start_offset_ == 0) {
            completed_round_.payload.move_from(result_.payload);
        } else if (!completed_round_.payload.append(result_.payload.data() +
                                                       round_start_offset_,
                                                   round_len)) {
            completed_round_.clear();
            return false;
        }
    }
    result_.payload.truncate(round_start_offset_);
    result_.bytes += static_cast<uint32_t>(round_len);
    status_.bytes = result_.bytes;
    status_.round_bytes = 0;
    completed_round_ready_ = true;
    return true;
}

void SpoolClient::continue_after_completed_round() {
    completed_round_.clear();
    completed_round_ready_ = false;
    if (result_.terminal_status == "SPOOL_COMPLETE_MORE_DATA_PENDING" &&
        result_.rounds < request_.max_rounds &&
        !next_spool_address_json_.empty()) {
        schedule_start();
        return;
    }

    if (result_.terminal_status == "SPOOL_COMPLETE_MORE_DATA_PENDING") {
        result_.truncated = true;
    }
    finish();
}

bool SpoolClient::retry_current_round(const char *reason) {
    if (round_retry_count_ >= AC_SPOOL_CLIENT_ROUND_RETRIES) {
        fail(reason);
        return false;
    }

    const uint32_t failed_spool_id = active_spool_id_;
    const uint16_t failed_round = result_.rounds;
    const uint32_t failed_round_fragments =
        static_cast<uint32_t>(round_fragment_count_);
    const size_t failed_round_bytes =
        result_.payload.size() >= round_start_offset_
            ? result_.payload.size() - round_start_offset_
            : 0;
    const uint32_t failed_idle_ms =
        state_started_ms_ ? millis() - state_started_ms_ : 0;

    result_.payload.truncate(round_start_offset_);
    result_.fragments = round_start_fragment_count_;
    if (!request_.stream_rounds) {
        result_.bytes = result_.payload.size();
    }
    result_.terminal_status.clear();
    result_.spool_hash.clear();
    result_.computed_sha256.clear();
    result_.sha_ok = false;
    result_.complete = false;
    if (result_.rounds > 0) result_.rounds--;

    round_fragment_count_ = 0;
    active_spool_id_ = 0;
    pending_id_ = 0;
    completed_round_.clear();
    completed_round_ready_ = false;
    round_retry_count_++;
    status_.error.clear();
    schedule_start();
    Log::logf(CAT_RPC, LOG_WARN,
              "[SPOOL] %s; retrying round %u/%u spool=%s round=%lu "
              "spool_id=%lu round_fragments=%u round_bytes=%lu "
              "total_bytes=%lu idle_ms=%lu\n",
              reason ? reason : "round_failed",
              static_cast<unsigned>(round_retry_count_),
              static_cast<unsigned>(AC_SPOOL_CLIENT_ROUND_RETRIES),
              result_.spool_type.c_str(),
              static_cast<unsigned long>(failed_round),
              static_cast<unsigned long>(failed_spool_id),
              static_cast<unsigned>(failed_round_fragments),
              static_cast<unsigned long>(failed_round_bytes),
              static_cast<unsigned long>(result_.payload.size()),
              static_cast<unsigned long>(failed_idle_ms));
    return true;
}

void SpoolClient::finish() {
    result_.complete = true;
    status_.state = SpoolClientState::Complete;
    status_.fragments = result_.fragments;
    status_.bytes = result_.bytes;
    state_ = State::Done;
    pending_id_ = 0;
    active_spool_id_ = 0;
    pending_submit_ = PendingSubmit::None;
}

void SpoolClient::fail(const char *message) {
    status_.state = SpoolClientState::Error;
    status_.error = message ? message : "error";
    state_ = State::Failed;
    Log::logf(CAT_RPC, LOG_WARN,
              "[SPOOL] %s spool=%s round=%lu spool_id=%lu "
              "round_fragments=%u round_bytes=%lu total_fragments=%lu "
              "total_bytes=%lu idle_ms=%lu\n",
              status_.error.c_str(),
              result_.spool_type.c_str(),
              static_cast<unsigned long>(result_.rounds),
              static_cast<unsigned long>(active_spool_id_),
              static_cast<unsigned>(round_fragment_count_),
              static_cast<unsigned long>(
                  result_.payload.size() - round_start_offset_),
              static_cast<unsigned long>(result_.fragments),
              static_cast<unsigned long>(result_.payload.size()),
              static_cast<unsigned long>(millis() - state_started_ms_));
    pending_id_ = 0;
    active_spool_id_ = 0;
    pending_submit_ = PendingSubmit::None;
}

void SpoolClient::update_status(uint32_t now_ms) {
    if (fetch_started_ms_) status_.elapsed_ms = now_ms - fetch_started_ms_;
    status_.current_round = result_.rounds;
    if (state_ == State::WaitStart) {
        status_.current_round = result_.rounds + 1;
    }
    status_.active_spool_id = active_spool_id_;
    status_.fragments = result_.fragments;
    status_.bytes = result_.bytes;
    status_.round_fragments = static_cast<uint32_t>(round_fragment_count_);
    const uint32_t round_bytes =
        result_.payload.size() >= round_start_offset_
            ? static_cast<uint32_t>(result_.payload.size() -
                                    round_start_offset_)
            : 0;
    status_.round_bytes = round_bytes;
    if (request_.stream_rounds) {
        status_.bytes += round_bytes;
    }
    status_.idle_ms = state_started_ms_ ? now_ms - state_started_ms_ : 0;
}

bool SpoolClient::take_completed_round(ReportSpoolResult &out) {
    if (!completed_round_ready_) return false;
    out.move_from(completed_round_);
    continue_after_completed_round();
    return true;
}

void SpoolClient::move_result_to(ReportSpoolResult &out) {
    out.move_from(result_);
}

std::string SpoolClient::build_start_params() const {
    std::string params = "{\"spoolAddress\":";
    if (!next_spool_address_json_.empty()) {
        params += next_spool_address_json_;
    } else {
        params += "{\"";
        params += request_.spool_type;
        params += "\":{\"fromDateTime\":\"";
        params += request_.from_dt;
        params += "\"}}";
    }
    params += ",\"maxSpoolSize\":";
    params += std::to_string(request_.max_size);
    params += "}";
    return params;
}

std::string SpoolClient::build_pull_params() const {
    std::string params = "{\"spoolId\":";
    params += std::to_string(active_spool_id_);
    params += ",\"maxFragmentSize\":";
    params += std::to_string(request_.fragment_max);
    params += ",\"maxNotifications\":";
    params += std::to_string(request_.max_notifications);
    params += "}";
    return params;
}

}  // namespace aircannect
