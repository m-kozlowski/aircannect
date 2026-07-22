#include "console_commands.h"

#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#include "as11_device_service.h"
#include "as11_rpc.h"
#include "as11_settings.h"
#include "as11_settings_manager.h"
#include "can_driver.h"
#include "edf_stream_signal_table.h"
#include "event_broker.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "rpc_request_port.h"
#include "rpc_transport_ports.h"
#include "stream_broker.h"
#include "string_util.h"
#include "time_sync_service.h"

namespace aircannect {
namespace {

bool json_number_literal(const String &value) {
    if (!value.length()) return false;

    const char first = value[0];
    if (first != '-' && !isdigit(static_cast<unsigned char>(first))) {
        return false;
    }

    bool digit = false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (isdigit(static_cast<unsigned char>(value[i]))) {
            digit = true;
            break;
        }
    }
    if (!digit) return false;

    char *end = nullptr;
    strtod(value.c_str(), &end);
    return end && end != value.c_str() && *end == '\0';
}

std::string cli_set_value_literal(String value) {
    value.trim();
    String lower = value;
    lower.toLowerCase();
    if (lower == "true" || lower == "false" || lower == "null" ||
        json_number_literal(value) || value.startsWith("{") ||
        value.startsWith("[")) {
        return to_std(value);
    }

    std::string out = "\"";
    out += json_escape(to_std(value));
    out += "\"";
    return out;
}

void append_cli_set_pair(std::string &out,
                         bool &first,
                         const String &key,
                         const String &value) {
    if (!first) out += ',';
    out += '"';
    out += json_escape(to_std(key));
    out += "\":";
    out += cli_set_value_literal(value);
    first = false;
}

void append_json_object_members(std::string &out,
                                bool &first,
                                const std::string &object) {
    if (object.size() < 2 || object.front() != '{' ||
        object.back() != '}') {
        return;
    }

    const size_t length = object.size() - 2;
    if (!length) return;
    if (!first) out += ',';
    out.append(object, 1, length);
    first = false;
}

void print_stream_memory_detail(Print &out, const StreamBroker &stream) {
    const size_t frame_pool_slots = stream.frame_pool_capacity();
    const size_t frame_pool_bytes =
        frame_pool_slots * sizeof(StreamFrameData) +
        frame_pool_slots * sizeof(StreamFrameData *);
    size_t queue_total = 0;
    size_t queue_capacity = 0;
    size_t queue_worst = 0;
    uint32_t queue_drops = 0;

    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        const StreamConsumerHandle handle =
            static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;

        const size_t queued = stream.consumer_queue_count(handle);
        queue_total += queued;
        queue_capacity += AC_STREAM_CONSUMER_QUEUE_DEPTH;
        if (queued > queue_worst) queue_worst = queued;
        queue_drops += stream.consumer_queue_drops(handle);
    }

    const char *pending = "none";
    if (stream.pending_start()) pending = "start";
    else if (stream.pending_stop()) pending = "stop";

    out.print("[MEM stream] desired=");
    out.print(stream.desired_active() ? "yes" : "no");
    out.print(" actual=");
    out.print(stream.actual_active() ? "yes" : "no");
    out.print(" pending=");
    out.print(pending);
    out.print(" consumers=");
    out.print(static_cast<unsigned long>(stream.consumer_count()));
    out.print(" q=");
    out.print(static_cast<unsigned long>(queue_total));
    out.print('/');
    out.print(static_cast<unsigned long>(queue_capacity));
    out.print(" q_worst=");
    out.print(static_cast<unsigned long>(queue_worst));
    out.print(" q_drops=");
    out.println(static_cast<unsigned long>(queue_drops));

    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        const StreamConsumerHandle handle =
            static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;

        out.print("[MEM stream consumer] id=");
        out.print(static_cast<unsigned long>(i));
        out.print(" source=");
        out.print(static_cast<unsigned long>(stream.consumer_source(handle)));
        out.print(" q=");
        out.print(static_cast<unsigned long>(
            stream.consumer_queue_count(handle)));
        out.print('/');
        out.print(static_cast<unsigned long>(AC_STREAM_CONSUMER_QUEUE_DEPTH));
        out.print(" drops=");
        out.println(static_cast<unsigned long>(
            stream.consumer_queue_drops(handle)));
    }

    out.print("[MEM owner] stream_frame_pool slots=");
    out.print(static_cast<unsigned long>(frame_pool_slots));
    out.print(" in_use=");
    out.print(static_cast<unsigned long>(stream.frame_pool_in_use()));
    out.print(" free=");
    out.print(static_cast<unsigned long>(stream.frame_pool_free()));
    out.print(" alloc_failures=");
    out.print(static_cast<unsigned long>(
        stream.frame_pool_allocation_failures()));
    out.print(" approx_bytes=");
    out.println(static_cast<unsigned long>(frame_pool_bytes));
}

void handle_stream(Print &out,
                   String rest,
                   StreamBroker &stream,
                   StreamConsumerHandle &handle) {
    rest.trim();

    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_stream_status(out, stream);
        return;
    }

    if (rest == "stop") {
        if (stream.consumer_active(handle)) {
            stream.release(handle);
            handle = STREAM_CONSUMER_INVALID;
            out.println("[STREAM] released console subscription");
        } else {
            out.println("[STREAM] no console subscription active");
        }
        ConsoleFormat::print_stream_status(out, stream);
        return;
    }

    if (rest.startsWith("{")) {
        StreamAcquireResult result;
        if (stream.consumer_active(handle)) {
            result = stream.update(handle, to_std(rest));
        } else {
            result = stream.acquire(to_std(rest), RpcSource::Console);
        }
        if (result.handle >= 0) handle = result.handle;

        if (result.status == StreamAcquireStatus::Incompatible) {
            out.println("[STREAM] params conflict with another consumer");
        } else if (result.status == StreamAcquireStatus::Full) {
            out.println("[STREAM] consumer table full");
        } else if (result.status == StreamAcquireStatus::Busy) {
            out.println("[STREAM] control request pending");
        } else if (result.status == StreamAcquireStatus::Rejected) {
            out.println("[STREAM] request rejected");
        } else {
            out.println("[STREAM] console subscription active");
        }
        ConsoleFormat::print_stream_status(out, stream);
        return;
    }

    std::string ids = DEFAULT_EDF_STREAM_IDS;
    uint32_t sample_ms = 40;
    uint32_t report_ms = 200;

    if (rest == "edf" || rest == "full" || rest == "default") {
        ids = DEFAULT_EDF_STREAM_IDS;
    } else if (rest.length()) {
        const int split = rest.indexOf(' ');
        ids = to_std(split < 0 ? rest : rest.substring(0, split));
        String tail = split < 0 ? "" : rest.substring(split + 1);
        tail.trim();

        if (tail.length()) {
            const int split2 = tail.indexOf(' ');
            const String sample =
                split2 < 0 ? tail : tail.substring(0, split2);
            sample_ms = strtoul(sample.c_str(), nullptr, 0);
            if (!sample_ms) sample_ms = 200;

            if (split2 >= 0) {
                String report = tail.substring(split2 + 1);
                report.trim();
                report_ms = report.length()
                                ? strtoul(report.c_str(), nullptr, 0)
                                : sample_ms * 5;
            } else {
                report_ms = sample_ms * 5;
            }
        }
    }

    const std::string params = build_stream_params(ids, sample_ms, report_ms);
    StreamAcquireResult result;
    if (stream.consumer_active(handle)) {
        result = stream.update(handle, params);
    } else {
        result = stream.acquire(params, RpcSource::Console);
    }
    if (result.handle >= 0) handle = result.handle;

    switch (result.status) {
        case StreamAcquireStatus::Acquired:
            out.println("[STREAM] StartStream queued");
            break;
        case StreamAcquireStatus::AlreadyActive:
            out.println("[STREAM] console subscription active");
            break;
        case StreamAcquireStatus::Incompatible:
            out.println("[STREAM] params conflict with another consumer");
            break;
        case StreamAcquireStatus::Full:
            out.println("[STREAM] consumer table full");
            break;
        case StreamAcquireStatus::Busy:
            out.println("[STREAM] control request pending");
            break;
        case StreamAcquireStatus::Rejected:
        default:
            out.println("[STREAM] request rejected");
            break;
    }
    ConsoleFormat::print_stream_status(out, stream);
}

void handle_time(Print &out,
                 String rest,
                 As11DeviceService &device,
                 TimeSyncService &time_sync) {
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        const time_t now = time(nullptr);
        struct tm utc = {};
        struct tm local = {};
        gmtime_r(&now, &utc);
        localtime_r(&now, &local);
        char utc_text[24];
        char local_text[24];
        strftime(utc_text, sizeof(utc_text), "%Y-%m-%d %H:%M:%S", &utc);
        strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S",
                 &local);

        out.print("[TIME] utc=");
        out.print(time_sync.esp_clock_valid() ? utc_text : "invalid");
        out.print(" local=");
        out.print(time_sync.esp_clock_valid() ? local_text : "invalid");
        out.print(" epoch=");
        out.print(static_cast<uint32_t>(now));
        out.print(" source=");
        out.print(time_sync.esp_clock_source_name());
        out.print(" ntp=");
        out.print(time_sync.ntp_synced() ? "synced" : "not_synced");
        out.print(" resmed_push=");
        out.print(time_sync.resmed_time_sync_enabled() ? "on" : "off");
        out.print(" resmed_offset_ms=");

        const As11DeviceState &as11 = device.state();
        if (as11.clock_offset_valid()) {
            out.print(as11.clock_offset_ms());
        } else {
            out.print("unknown");
        }
        out.print(" status=");
        out.println(time_sync.last_status());
        return;
    }

    if (rest == "get") {
        (void)time_sync.request_pull_resmed_to_esp(RpcSource::Console);
        return;
    }
    if (rest == "set" || rest == "push" || rest == "sync-to-resmed") {
        if (time_sync.request_push_esp_to_resmed(RpcSource::Console)) {
            out.println("[TIME] SetDateTime queued");
        } else {
            out.println("[TIME] ESP clock is not ready or queue is full");
        }
        return;
    }
    if (rest == "pull" || rest == "sync-from-resmed") {
        if (time_sync.request_pull_resmed_to_esp(RpcSource::Console)) {
            out.println("[TIME] GetDateTime queued for ESP clock sync");
        } else {
            out.println("[TIME] GetDateTime queue failed");
        }
        return;
    }
    if (rest == "ntp") {
        time_sync.force_ntp_sync();
        out.println("[TIME] NTP resync triggered");
        return;
    }

    print_unknown_command(out, "TIME", "time, get, push, pull, ntp");
}

}  // namespace

As11ConsoleCommands::As11ConsoleCommands(
    RpcRequestPort &rpc,
    RpcPassthroughPort &passthrough,
    RpcDiagnosticsPort &diagnostics,
    CanDriver &can,
    EventBroker &events,
    StreamBroker &stream,
    As11DeviceService &device,
    As11SettingsManager &settings,
    TimeSyncService &time_sync)
    : rpc_(rpc),
      passthrough_(passthrough),
      diagnostics_(diagnostics),
      can_(can),
      events_(events),
      stream_(stream),
      device_(device),
      settings_(settings),
      time_sync_(time_sync) {}

As11ConsoleCommands::StreamSessionState *
As11ConsoleCommands::stream_session(uint32_t session_id, bool create) {
    StreamSessionState *empty = nullptr;
    for (StreamSessionState &session : stream_sessions_) {
        if (session.session_id == session_id) return &session;
        if (!session.session_id && !empty) empty = &session;
    }
    if (!create || !empty) return nullptr;

    empty->session_id = session_id;
    empty->handle = STREAM_CONSUMER_INVALID;
    return empty;
}

bool As11ConsoleCommands::execute(const String &command,
                                  const String &rest_arg,
                                  Print &out,
                                  ConsoleCommandSession &session) {
    if (command != "as11" && command != "therapy" && command != "time" &&
        command != "get" && command != "set" && command != "stream" &&
        command != "rpc" && command != "raw" && command != "can") {
        return false;
    }

    String rest = rest_arg;

    if (command == "as11") {
        rest.trim();
        if (!rest.length() || rest == "status") {
            ConsoleFormat::print_as11_status(out, device_.state());
        } else if (rest == "poll" || rest == "refresh") {
            device_.request_healthcheck(rpc_, RpcSource::Console, millis());
            out.println("[AS11] healthcheck scheduled");
        } else if (rest == "version") {
            passthrough_.send_request("GetVersion", "", RpcSource::Console);
        } else {
            print_unknown_command(out, "AS11", "as11 status, poll, version");
        }
        return true;
    }

    if (command == "therapy") {
        rest.trim();
        rest.toLowerCase();
        if (!rest.length() || rest == "status") {
            ConsoleFormat::print_as11_status(out, device_.state());
        } else if (rest == "start" || rest == "on" || rest == "run") {
            const bool accepted = device_.request_therapy(
                rpc_, As11TherapyTarget::Running, RpcSource::Console,
                millis()).accepted();
            out.println(accepted ? "[THERAPY] EnterTherapy queued"
                                 : "[THERAPY] EnterTherapy queue failed");
        } else if (rest == "stop" || rest == "off" ||
                   rest == "standby") {
            const bool accepted = device_.request_therapy(
                rpc_, As11TherapyTarget::Standby, RpcSource::Console,
                millis()).accepted();
            out.println(accepted ? "[THERAPY] EnterStandby queued"
                                 : "[THERAPY] EnterStandby queue failed");
        } else {
            print_unknown_command(out, "THERAPY",
                                  "therapy status, start, stop");
        }
        return true;
    }

    if (command == "time") {
        handle_time(out, rest, device_, time_sync_);
        return true;
    }

    if (command == "get") {
        rest.trim();
        if (!rest.length()) {
            out.println("[RPC] usage: get NAME [NAME...]");
        } else {
            passthrough_.send_request("Get", build_get_params(to_std(rest)),
                                      RpcSource::Console);
        }
        return true;
    }

    if (command == "set") {
        rest.trim();
        if (!rest.length()) {
            out.println(
                "[RPC] usage: set NAME VALUE [NAME VALUE...] | "
                "set {JSON_PARAMS}");
            return true;
        }

        std::string params;
        bool managed_settings = false;
        if (rest.startsWith("{")) {
            params = to_std(rest);
        } else {
            int pos = 0;
            String key;
            String value;
            std::string raw_params = "{";
            std::string setting_body = "{";
            bool raw_first = true;
            bool setting_first = true;
            size_t raw_count = 0;
            size_t setting_count = 0;

            while (parse_console_arg(rest, pos, key)) {
                if (!parse_console_arg(rest, pos, value)) {
                    out.println(
                        "[RPC] usage: set NAME VALUE [NAME VALUE...] | "
                        "set {JSON_PARAMS}");
                    return true;
                }

                if (key.startsWith("_")) {
                    append_cli_set_pair(raw_params, raw_first, key, value);
                    raw_count++;
                } else {
                    append_cli_set_pair(setting_body, setting_first, key,
                                        value);
                    setting_count++;
                }
            }
            raw_params += '}';
            setting_body += '}';

            const As11SettingsState &settings = settings_.state();
            const As11DeviceState &as11 = device_.state();
            int mode = settings.mode_index();
            if (mode < 0) {
                mode = as11_mode_index_from_value(
                    as11.active_therapy_profile());
            }

            size_t accepted = 0;
            std::string mapped_params = "{}";
            if (setting_count) {
                mapped_params = as11_build_set_params_from_json(
                    setting_body, mode, accepted);
            }
            if (!raw_count && !accepted) {
                out.println("[RPC] no accepted settings");
                return true;
            }
            managed_settings = accepted != 0;

            bool first = true;
            params = '{';
            append_json_object_members(params, first, raw_params);
            append_json_object_members(params, first, mapped_params);
            params += '}';
        }

        const bool queued = managed_settings
            ? settings_.write(rpc_, params, RpcSource::Console,
                              millis()).accepted()
            : passthrough_.send_request("Set", params, RpcSource::Console);
        out.println(queued ? "[RPC] Set queued" : "[RPC] Set queue failed");
        return true;
    }

    if (command == "stream") {
        StreamSessionState *stream_state =
            stream_session(session.id, true);
        if (!stream_state) {
            out.println("[STREAM] console session table full");
            return true;
        }

        handle_stream(out, rest, stream_, stream_state->handle);
        return true;
    }

    if (command == "rpc") {
        rest.trim();
        const int split = rest.indexOf(' ');
        String method = split < 0 ? rest : rest.substring(0, split);
        String params = split < 0 ? "" : rest.substring(split + 1);
        params.trim();
        if (!method.length()) {
            out.println("[RPC] usage: rpc METHOD [JSON_PARAMS]");
        } else {
            passthrough_.send_request(to_std(method), to_std(params),
                                      RpcSource::Console);
        }
        return true;
    }

    if (command == "raw") {
        rest.trim();
        if (!rest.length()) {
            out.println("[RPC] usage: raw JSON");
        } else {
            passthrough_.submit_raw_payload(to_std(rest), RpcSource::Console);
        }
        return true;
    }

    if (command == "can") {
        rest.trim();
        rest.toLowerCase();
        if (!rest.length() || rest == "status") {
            ConsoleFormat::print_rpc_status(out, diagnostics_, can_);
        } else if (rest == "restart") {
            diagnostics_.recover_can("console CAN restart command");
        } else {
            print_unknown_command(out, "CAN", "can status, can restart");
        }
        return true;
    }

    return false;
}

void As11ConsoleCommands::stop(ConsoleCommandSession &session) {
    StreamSessionState *stream_state = stream_session(session.id, false);
    if (!stream_state) return;

    if (stream_.consumer_active(stream_state->handle)) {
        stream_.release(stream_state->handle);
    }
    *stream_state = {};
}

void As11ConsoleCommands::print_status(Print &out) {
    ConsoleFormat::print_rpc_status(out, diagnostics_, can_);
    ConsoleFormat::print_as11_status(out, device_.state());
}

void As11ConsoleCommands::print_stats(Print &out) {
    ConsoleFormat::print_rpc_stats(out, diagnostics_, can_, events_, stream_);
}

void As11ConsoleCommands::reset_stats() {
    diagnostics_.reset_stats();
    events_.reset_counters();
    stream_.reset_counters();
}

void As11ConsoleCommands::print_memory_detail(Print &out) {
    print_stream_memory_detail(out, stream_);
}

}  // namespace aircannect
