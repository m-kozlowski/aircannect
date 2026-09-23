#include "airmini_history_data.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string.h>
#include <utility>

#include "edf_file_writer.h"
#include "edf_day_statistics.h"
#include "edf_str_settings.h"
#include "edf_str_signal_table.h"
#include "edf_storage_catalog.h"
#include "edf_time.h"
#include "large_json_allocator.h"
#include "utc_time.h"

namespace aircannect {
namespace {

static constexpr uint32_t HISTORY_SCHEMA = 1;
static constexpr char USAGE_DATA_ID[] =
    "UsageEvents-TherapyStatusEvent";
static constexpr char RESPIRATORY_DATA_ID[] =
    "TherapyEvents-RespiratoryEvent";
static constexpr char PRESSURE_DATA_ID[] =
    "TherapyOneMinutePeriodic-InspiratoryPressure";
static constexpr char LEAK_DATA_ID[] =
    "TherapyOneMinutePeriodic-Leak";

template <typename Text>
class JsonTextWriter {
public:
    explicit JsonTextWriter(Text &text) : text_(text) {}

    size_t write(uint8_t value) {
        const char character = static_cast<char>(value);
        return text_.append(&character, 1), 1;
    }

    size_t write(const uint8_t *data, size_t size) {
        if (!data || size == 0) return 0;
        text_.append(reinterpret_cast<const char *>(data), size);
        return size;
    }

    bool ok() const { return !failed_; }

private:
    Text &text_;
    bool failed_ = false;
};

const char *selector_name(AirMiniHistorySelector selector) {
    switch (selector) {
        case AirMiniHistorySelector::UsageEvents: return USAGE_DATA_ID;
        case AirMiniHistorySelector::RespiratoryEvents:
            return RESPIRATORY_DATA_ID;
        case AirMiniHistorySelector::InspiratoryPressure: return PRESSURE_DATA_ID;
        case AirMiniHistorySelector::Leak: return LEAK_DATA_ID;
        case AirMiniHistorySelector::Count: break;
    }
    return nullptr;
}

AirMiniHistorySelector selector_from_name(const char *name) {
    if (!name) return AirMiniHistorySelector::Count;

    for (uint8_t i = 0; i < static_cast<uint8_t>(AirMiniHistorySelector::Count);
         ++i) {
        const auto selector = static_cast<AirMiniHistorySelector>(i);
        if (strcmp(name, selector_name(selector)) == 0) return selector;
    }
    return AirMiniHistorySelector::Count;
}

bool valid_selector(AirMiniHistorySelector selector) {
    return static_cast<uint8_t>(selector) <
           static_cast<uint8_t>(AirMiniHistorySelector::Count);
}

bool valid_range(const AirMiniHistoryRange &range) {
    return (range.from_ms == 0 && range.to_ms == 0) ||
           (range.from_ms < range.to_ms);
}

bool in_range(int64_t timestamp_ms, const AirMiniHistoryRange &range) {
    if (range.from_ms == 0 && range.to_ms == 0) return true;
    return timestamp_ms >= range.from_ms && timestamp_ms < range.to_ms;
}

bool parse_uint32(JsonVariantConst value, uint32_t &out) {
    if (!value.is<uint32_t>()) return false;
    out = value.as<uint32_t>();
    return true;
}

bool parse_int64(JsonVariantConst value, int64_t &out) {
    if (!value.is<int64_t>()) return false;
    out = value.as<int64_t>();
    return true;
}

bool parse_complete(JsonVariantConst value, bool &complete) {
    if (value.is<bool>()) {
        complete = value.as<bool>();
        return true;
    }

    const char *text = value.as<const char *>();
    if (!text) return false;
    if (strcmp(text, "true") == 0) {
        complete = true;
        return true;
    }
    if (strcmp(text, "false") == 0) {
        complete = false;
        return true;
    }
    return false;
}

bool parse_wire_time(JsonVariantConst value,
                     int64_t &timestamp_ms) {
    const char *text = value.as<const char *>();
    return text && parse_utc_iso8601_ms(text, timestamp_ms);
}

AirMiniHistoryEventKind event_kind(const char *name) {
    if (!name) return AirMiniHistoryEventKind::Unknown;
    if (strcmp(name, "MaskOn") == 0) return AirMiniHistoryEventKind::MaskOn;
    if (strcmp(name, "MaskOff") == 0) return AirMiniHistoryEventKind::MaskOff;
    if (strcmp(name, "PowerOff") == 0) return AirMiniHistoryEventKind::PowerOff;
    if (strcmp(name, "HypopneaEnd") == 0) {
        return AirMiniHistoryEventKind::HypopneaEnd;
    }
    if (strcmp(name, "CentralApneaEnd") == 0) {
        return AirMiniHistoryEventKind::CentralApneaEnd;
    }
    if (strcmp(name, "ObstructiveApneaEnd") == 0) {
        return AirMiniHistoryEventKind::ObstructiveApneaEnd;
    }
    if (strcmp(name, "ApneaEnd") == 0) return AirMiniHistoryEventKind::ApneaEnd;
    if (strcmp(name, "ReraEnd") == 0) return AirMiniHistoryEventKind::ReraEnd;
    if (strcmp(name, "CsrStart") == 0) return AirMiniHistoryEventKind::CsrStart;
    if (strcmp(name, "CsrEnd") == 0) return AirMiniHistoryEventKind::CsrEnd;
    return AirMiniHistoryEventKind::Unknown;
}

const char *event_name(AirMiniHistoryEventKind kind) {
    switch (kind) {
        case AirMiniHistoryEventKind::MaskOn: return "MaskOn";
        case AirMiniHistoryEventKind::MaskOff: return "MaskOff";
        case AirMiniHistoryEventKind::PowerOff: return "PowerOff";
        case AirMiniHistoryEventKind::HypopneaEnd: return "HypopneaEnd";
        case AirMiniHistoryEventKind::CentralApneaEnd:
            return "CentralApneaEnd";
        case AirMiniHistoryEventKind::ObstructiveApneaEnd:
            return "ObstructiveApneaEnd";
        case AirMiniHistoryEventKind::ApneaEnd: return "ApneaEnd";
        case AirMiniHistoryEventKind::ReraEnd: return "ReraEnd";
        case AirMiniHistoryEventKind::CsrStart: return "CsrStart";
        case AirMiniHistoryEventKind::CsrEnd: return "CsrEnd";
        case AirMiniHistoryEventKind::Unknown: break;
    }
    return "Unknown";
}

bool copy_name(char *out, size_t capacity, const char *name) {
    if (!out || capacity == 0 || !name || !name[0]) return false;
    const size_t length = strlen(name);
    if (length >= capacity) return false;
    memcpy(out, name, length + 1);
    return true;
}

bool profile_less(const AirMiniHistorySettingProfile &lhs,
                  const AirMiniHistorySettingProfile &rhs) {
    return lhs.applied_time_ms < rhs.applied_time_ms;
}

AirMiniHistoryDecodeResult decode_result(bool ok,
                                         bool changed,
                                         AirMiniHistoryDecodeError error) {
    AirMiniHistoryDecodeResult result;
    result.ok = ok;
    result.state_changed = changed;
    result.error = error;
    return result;
}

bool state_is_terminal(AirMiniHistoryTransferState state) {
    return state == AirMiniHistoryTransferState::Complete ||
           state == AirMiniHistoryTransferState::Rejected;
}

bool parse_id(const JsonDocument &document, uint32_t expected) {
    uint32_t actual = 0;
    return parse_uint32(document["id"], actual) && actual == expected;
}

bool all_logged_terminal(const AirMiniHistorySeries *series,
                         const bool *requested) {
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        if (requested[i] && !state_is_terminal(series[i].state)) {
            return false;
        }
    }
    return true;
}

bool append_sample(AirMiniHistorySeries &series,
                   const AirMiniHistoryRange &range,
                   const AirMiniHistoryLimits &limits,
                   int64_t timestamp_ms,
                   float value) {
    if (!std::isfinite(value)) return false;
    if (!in_range(timestamp_ms, range)) return true;

    if (!series.samples.empty() &&
        series.samples.back().timestamp_ms < timestamp_ms) {
        if (series.samples.size() >= limits.samples_per_selector) {
            return false;
        }
        try {
            series.samples.push_back({timestamp_ms, value});
        } catch (const std::bad_alloc &) {
            return false;
        }
        series.has_data = true;
        return true;
    }

    auto position = std::lower_bound(
        series.samples.begin(), series.samples.end(), timestamp_ms,
        [](const AirMiniHistorySample &sample, int64_t timestamp) {
            return sample.timestamp_ms < timestamp;
        });
    if (position != series.samples.end() &&
        position->timestamp_ms == timestamp_ms) {
        position->value = value;
        series.has_data = true;
        return true;
    }
    if (series.samples.size() >= limits.samples_per_selector) {
        return false;
    }
    try {
        series.samples.insert(position, {timestamp_ms, value});
    } catch (const std::bad_alloc &) {
        return false;
    }
    series.has_data = true;
    return true;
}

bool append_event(AirMiniHistorySeries &series,
                  const AirMiniHistoryRange &range,
                  const AirMiniHistoryLimits &limits,
                  const AirMiniHistoryEvent &event) {
    if (!in_range(event.timestamp_ms, range)) return true;

    if (series.events.size() >= limits.events) return false;
    try {
        if (series.events.empty() ||
            series.events.back().timestamp_ms <= event.timestamp_ms) {
            series.events.push_back(event);
        } else {
            const auto position = std::upper_bound(
                series.events.begin(), series.events.end(),
                event.timestamp_ms,
                [](int64_t timestamp, const AirMiniHistoryEvent &value) {
                    return timestamp < value.timestamp_ms;
                });
            series.events.insert(position, event);
        }
    } catch (const std::bad_alloc &) {
        return false;
    }
    series.has_data = true;
    return true;
}

bool parse_logged_events(AirMiniHistorySeries &series,
                         const AirMiniHistoryRange &range,
                         const AirMiniHistoryLimits &limits,
                         JsonArrayConst events) {
    for (JsonObjectConst object : events) {
        const char *name = object["event"].as<const char *>();
        if (!name || !name[0]) return false;

        AirMiniHistoryEvent event;
        if (!parse_wire_time(object["time"].isNull()
                                 ? object["reportTime"]
                                 : object["time"],
                             event.timestamp_ms)) {
            return false;
        }
        if (!copy_name(event.name, sizeof(event.name), name)) return false;
        event.kind = event_kind(name);

        if (!object["durationSeconds"].isNull()) {
            if (!parse_uint32(object["durationSeconds"],
                              event.duration_seconds)) {
                return false;
            }
            event.has_duration = true;
        }
        if (!object["backdateSeconds"].isNull()) {
            if (!parse_uint32(object["backdateSeconds"],
                              event.backdate_seconds)) {
                return false;
            }
            event.has_backdate = true;
        }
        if (!append_event(series, range, limits, event)) return false;
    }
    return true;
}

bool parse_periodic(AirMiniHistorySeries &series,
                    const AirMiniHistoryRange &range,
                    const AirMiniHistoryLimits &limits,
                    JsonObjectConst periodic) {
    const JsonArrayConst values = periodic["values"].as<JsonArrayConst>();
    if (values.isNull()) return false;
    if (values.size() == 0) return true;

    int64_t start_ms = 0;
    if (!parse_wire_time(periodic["startTime"], start_ms)) {
        return false;
    }
    const double interval_seconds = periodic["interval"] | 0.0;
    if (!std::isfinite(interval_seconds) || interval_seconds <= 0.0 ||
        interval_seconds > 86400.0) {
        return false;
    }
    const int64_t interval_ms = static_cast<int64_t>(
        std::llround(interval_seconds * 1000.0));
    if (interval_ms <= 0) return false;

    for (size_t i = 0; i < values.size(); ++i) {
        if (i > static_cast<size_t>(INT64_MAX / interval_ms) ||
            start_ms > INT64_MAX -
                           static_cast<int64_t>(i) * interval_ms) {
            return false;
        }
        const JsonVariantConst value = values[i];
        if (value.isNull()) continue;
        if (!value.is<float>()) return false;
        const float parsed = value.as<float>();
        if (!append_sample(series, range, limits,
                           start_ms + static_cast<int64_t>(i) * interval_ms,
                           parsed)) {
            return false;
        }
    }
    return true;
}

bool append_profile(AirMiniHistorySettings &settings,
                    const AirMiniHistoryRange &range,
                    const AirMiniHistoryLimits &limits,
                    JsonObjectConst object) {
    (void)range;
    const JsonObjectConst attributes = object["Attributes"].as<JsonObjectConst>();
    int64_t timestamp_ms = 0;
    if (attributes.isNull() ||
        !parse_wire_time(attributes["AppliedDateTime"], timestamp_ms)) {
        return false;
    }
    try {
        AirMiniHistoryText json;
        JsonTextWriter<AirMiniHistoryText> writer(json);
        if (serializeJson(object, writer) == 0 || json.size() == 0 ||
            json.size() > limits.settings_json_bytes) {
            return false;
        }
        if (settings.profiles.size() >= limits.settings_profiles) {
            return false;
        }

        AirMiniHistorySettingProfile profile;
        profile.applied_time_ms = timestamp_ms;
        profile.json = std::move(json);
        if (settings.profiles.empty() ||
            settings.profiles.back().applied_time_ms <= timestamp_ms) {
            settings.profiles.push_back(std::move(profile));
        } else {
            const auto position = std::upper_bound(
                settings.profiles.begin(), settings.profiles.end(),
                timestamp_ms,
                [](int64_t timestamp,
                   const AirMiniHistorySettingProfile &value) {
                    return timestamp < value.applied_time_ms;
                });
            settings.profiles.insert(position, std::move(profile));
        }
    } catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

void merge_state(AirMiniHistoryTransferState &target,
                 AirMiniHistoryTransferState incoming) {
    if (incoming != AirMiniHistoryTransferState::NotRequested) target = incoming;
}

// Merge sorted timestamp groups. A repeat transfer keeps the maximum
// multiplicity of identical records, not their sum; distinct equal-time
// events retain their original order.
template <typename Rows, typename Time, typename Equal>
bool merge_retained_rows(Rows &target, const Rows &source, size_t limit,
                         Time time, Equal equal) {
    Rows merged;
    merged.reserve(std::min(limit, target.size() + source.size()));
    size_t left = 0;
    size_t right = 0;
    while (left < target.size() || right < source.size()) {
        if (right == source.size() ||
            (left < target.size() && time(target[left]) < time(source[right]))) {
            merged.push_back(target[left++]);
        } else if (left == target.size() || time(source[right]) < time(target[left])) {
            merged.push_back(source[right++]);
        } else {
            const size_t first_left = left;
            const size_t first_right = right;
            const auto timestamp = time(target[left]);
            while (left < target.size() && time(target[left]) == timestamp) {
                merged.push_back(target[left++]);
            }
            while (right < source.size() && time(source[right]) == timestamp) {
                size_t retained = 0;
                size_t received = 1;
                for (size_t i = first_left; i < left; ++i) {
                    if (equal(target[i], source[right])) ++retained;
                }
                for (size_t i = first_right; i < right; ++i) {
                    if (equal(source[i], source[right])) ++received;
                }
                if (received > retained) merged.push_back(source[right]);
                ++right;
            }
        }
        if (merged.size() > limit) return false;
    }
    target.swap(merged);
    return true;
}

bool event_is_mask_context(AirMiniHistoryEventKind kind) {
    return kind == AirMiniHistoryEventKind::MaskOn ||
           kind == AirMiniHistoryEventKind::MaskOff ||
           kind == AirMiniHistoryEventKind::PowerOff;
}

bool history_time_to_utc(int64_t stored_ms,
                         const AirMiniHistoryProjectionInput &input,
                         int64_t &utc_ms);

bool latest_mapped_history_time(
    const AirMiniHistorySeries *series,
    const AirMiniHistoryProjectionInput &input,
    int64_t &result) {
    bool found = false;
    result = 0;
    auto consider = [&](int64_t stored_ms) {
        int64_t mapped_ms = 0;
        if (!history_time_to_utc(stored_ms, input, mapped_ms)) return;
        if (!found || mapped_ms > result) result = mapped_ms;
        found = true;
    };
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        for (const auto &sample : series[i].samples) {
            consider(sample.timestamp_ms);
        }
        for (const auto &event : series[i].events) {
            consider(event.timestamp_ms);
        }
    }
    return found;
}

bool add_session(AirMiniHistoryStrProjection &out,
                 int64_t start_ms,
                 int64_t end_ms,
                 bool closed_at_observed_end,
                 int64_t day_start_ms,
                 int64_t day_end_ms) {
    start_ms = std::max(start_ms, day_start_ms);
    end_ms = std::min(end_ms, day_end_ms);
    if (end_ms <= start_ms) return true;

    try {
        out.sessions.push_back({start_ms, end_ms, closed_at_observed_end});
        std::stable_sort(
            out.sessions.begin(), out.sessions.end(),
            [](const AirMiniHistorySession &left,
               const AirMiniHistorySession &right) {
                return left.start_ms < right.start_ms;
            });

        size_t merged_count = 0;
        for (const auto &current : out.sessions) {
            if (merged_count == 0 ||
                current.start_ms > out.sessions[merged_count - 1].end_ms) {
                out.sessions[merged_count++] = current;
                continue;
            }

            AirMiniHistorySession &merged = out.sessions[merged_count - 1];
            if (current.end_ms > merged.end_ms) {
                merged.end_ms = current.end_ms;
                merged.closed_at_observed_end =
                    current.closed_at_observed_end;
            } else if (current.end_ms == merged.end_ms) {
                merged.closed_at_observed_end =
                    merged.closed_at_observed_end &&
                    current.closed_at_observed_end;
            }
        }
        out.sessions.resize(merged_count);
    } catch (const std::bad_alloc &) {
        return false;
    }

    out.duration_ms = 0;
    out.mask_event_count = 0;
    out.has_unclosed_history = false;
    for (const auto &session : out.sessions) {
        const uint64_t duration = static_cast<uint64_t>(
            session.end_ms - session.start_ms);
        out.duration_ms = UINT64_MAX - out.duration_ms < duration
            ? UINT64_MAX : out.duration_ms + duration;
        ++out.mask_event_count;
        out.has_unclosed_history |= session.closed_at_observed_end;
    }
    return true;
}

bool history_time_to_utc(int64_t stored_ms,
                         const AirMiniHistoryProjectionInput &input,
                         int64_t &utc_ms) {
    if (input.local_statistics &&
        edf_day_statistics_map_raw_time(*input.local_statistics,
                                         stored_ms, utc_ms)) {
        return true;
    }

    if (input.clock_window_start_ms >= input.clock_window_end_ms ||
        input.clock_window_start_ms == 0) {
        // History timestamps are already on the device's exported epoch. A
        // clock transform is only allowed when the caller bounds the window
        // for which that transform was sampled.
        utc_ms = stored_ms;
        return true;
    }

    int64_t raw_start_ms = input.clock_window_start_ms;
    int64_t raw_end_ms = input.clock_window_end_ms;
    if (input.clock.externally_referenced) {
        const int64_t correction = input.clock.device_minus_utc_ms;
        if ((correction > 0 &&
             (raw_start_ms > INT64_MAX - correction ||
              raw_end_ms > INT64_MAX - correction)) ||
            (correction < 0 &&
             (raw_start_ms < INT64_MIN - correction ||
              raw_end_ms < INT64_MIN - correction))) {
            return false;
        }
        raw_start_ms += correction;
        raw_end_ms += correction;
    }
    if (stored_ms < raw_start_ms || stored_ms >= raw_end_ms) {
        // The sampled transform is intentionally local to its known window.
        // Older context records remain usable on their exported epoch rather
        // than disappearing merely because their offset is unknown.
        utc_ms = stored_ms;
        return true;
    }
    return input.clock.to_utc_ms(stored_ms, utc_ms);
}

bool latest_observation_before(const AirMiniHistorySeries *series,
                               size_t series_count,
                               const AirMiniHistoryProjectionInput &input,
                               int64_t before_ms,
                               int64_t minimum_ms,
                               int64_t &result) {
    bool found = false;
    result = minimum_ms;
    auto consider = [&](int64_t stored_ms) {
        int64_t mapped_ms = 0;
        if (!history_time_to_utc(stored_ms, input, mapped_ms) ||
            mapped_ms < minimum_ms || mapped_ms >= before_ms) {
            return;
        }
        if (!found || mapped_ms > result) result = mapped_ms;
        found = true;
    };

    for (size_t i = 0; i < series_count; ++i) {
        const auto &samples = series[i].samples;
        const auto sample_it = std::lower_bound(
            samples.begin(), samples.end(), before_ms,
            [](const AirMiniHistorySample &sample, int64_t time) {
                return sample.timestamp_ms < time;
            });
        if (sample_it != samples.begin()) consider((sample_it - 1)->timestamp_ms);

        const auto &events = series[i].events;
        const auto event_it = std::lower_bound(
            events.begin(), events.end(), before_ms,
            [](const AirMiniHistoryEvent &event, int64_t time) {
                return event.timestamp_ms < time;
            });
        if (event_it != events.begin()) consider((event_it - 1)->timestamp_ms);
    }
    return found;
}

bool collect_history_sessions(const AirMiniHistorySeries *series,
                              size_t series_count,
                              const AirMiniHistoryProjectionInput &input,
                              int64_t observed_end_ms,
                              AirMiniHistoryStrProjection &out) {
    const AirMiniHistorySeries &usage =
        series[static_cast<size_t>(AirMiniHistorySelector::UsageEvents)];
    bool open = false;
    int64_t start_ms = 0;
    for (const auto &stored_event : usage.events) {
        int64_t event_ms = 0;
        if (!history_time_to_utc(stored_event.timestamp_ms, input,
                                 event_ms)) {
            continue;
        }
        if (event_ms >= input.day_end_ms) continue;
        if (stored_event.kind == AirMiniHistoryEventKind::MaskOn) {
            if (open) {
                int64_t close_ms = start_ms;
                (void)latest_observation_before(
                    series, series_count, input, event_ms, start_ms, close_ms);
                if (close_ms > start_ms &&
                    !add_session(out, start_ms, close_ms, false,
                                 input.day_start_ms, input.day_end_ms)) {
                    return false;
                }
            }
            open = true;
            start_ms = event_ms;
            continue;
        }
        if (!open) continue;
        if (stored_event.kind == AirMiniHistoryEventKind::MaskOff ||
            stored_event.kind == AirMiniHistoryEventKind::PowerOff) {
            if (!add_session(out, start_ms, event_ms, false,
                             input.day_start_ms, input.day_end_ms)) {
                return false;
            }
            open = false;
        }
    }
    if (open && observed_end_ms >= start_ms) {
        return add_session(out, start_ms, observed_end_ms, true,
                           input.day_start_ms, input.day_end_ms);
    }
    return true;
}

enum class StrIntervalKind { Therapy, Mask, TherapyAndMask };

bool apply_session_to_str(const AirMiniHistorySession &session,
                          int32_t timezone_offset_minutes,
                          EdfStrSessionAccumulator &str,
                          StrIntervalKind kind = StrIntervalKind::TherapyAndMask) {
    EdfLocalDateTime start;
    EdfLocalDateTime end;
    if (!edf_epoch_ms_to_local_datetime(session.start_ms,
                                        timezone_offset_minutes, start) ||
        !edf_epoch_ms_to_local_datetime(session.end_ms,
                                        timezone_offset_minutes, end)) {
        return false;
    }

    EdfStrSessionStatus status = EdfStrSessionStatus::Ok;
    if (kind != StrIntervalKind::Mask) {
        if (!str.begin_therapy(start, status)) return false;
        bool record_ready = false;
        if (!str.finish_therapy(end, record_ready, status)) return false;
    }
    if (kind != StrIntervalKind::Therapy) {
        if (!str.begin_mask_event(start, status)) return false;
        if (!str.finish_mask_event(end, status)) return false;
    }
    return true;
}

int str_signal_for_tag(const char *tag) {
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *descriptor =
            edf_str_signal_descriptor(i);
        if (descriptor && descriptor->short_tag &&
            strcmp(descriptor->short_tag, tag) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool coverage_covers_sessions(const EdfDayCoverage &coverage,
                             const AirMiniHistorySessionList &sessions);

void apply_stat_value(const EdfDaySignalStatistics &entry,
                      int value_milli,
                      const char *tag,
                      float canonical_to_str_divisor,
                      EdfStrSessionAccumulator &str) {
    if (!entry.valid) return;
    const int index = str_signal_for_tag(tag);
    if (index >= 0) {
        (void)str.set_signal_physical(static_cast<size_t>(index),
            static_cast<float>(value_milli) / (1000.0f * canonical_to_str_divisor));
    }
}

bool coverage_covers_interval(const EdfDayCoverage &coverage,
                              int64_t start_ms,
                              int64_t end_ms) {
    if (end_ms <= start_ms) return true;
    int64_t cursor = start_ms;
    for (size_t i = 0; i < coverage.count; ++i) {
        const EdfDayCoverageInterval &interval = coverage.intervals[i];
        if (interval.end_ms <= cursor) continue;
        if (interval.start_ms > cursor) return false;
        cursor = std::max(cursor, interval.end_ms);
        if (cursor >= end_ms) return true;
    }
    return false;
}

bool coverage_covers_sessions(const EdfDayCoverage &coverage,
                             const AirMiniHistorySessionList &sessions) {
    if (sessions.empty()) return false;
    for (const auto &session : sessions) {
        if (!coverage_covers_interval(coverage, session.start_ms,
                                      session.end_ms)) {
            return false;
        }
    }
    return true;
}

bool add_clipped_history_sample(const AirMiniHistoryStrProjection &projection,
                                EdfDayStatisticsReader &reader,
                                ReportSignalId signal,
                                const AirMiniHistorySample &sample) {
    const int64_t sample_end = sample.timestamp_ms > INT64_MAX - 60000
        ? INT64_MAX : sample.timestamp_ms + 60000;
    int64_t cursor = sample.timestamp_ms;
    while (cursor < sample_end) {
        int64_t next_start = sample_end;
        int64_t covered_end = cursor;
        for (const auto &session : projection.sessions) {
            if (session.end_ms <= cursor ||
                session.start_ms >= sample_end) {
                continue;
            }
            if (session.start_ms <= cursor) {
                covered_end = std::max(
                    covered_end, std::min(session.end_ms, sample_end));
            } else {
                next_start = std::min(next_start, session.start_ms);
            }
        }
        if (covered_end > cursor) {
            if (!reader.add_fallback_sample(
                    signal, sample.value, cursor,
                    static_cast<uint32_t>(covered_end - cursor))) {
                return false;
            }
            cursor = covered_end;
        } else if (next_start < sample_end) {
            cursor = next_start;
        } else {
            break;
        }
    }
    return true;
}

bool add_history_fallback(const AirMiniHistoryStrProjection &projection,
                          EdfDayStatisticsReader &reader) {
    if (reader.status().state != EdfDayStatisticsState::Complete &&
        reader.status().state != EdfDayStatisticsState::Finalize) {
        return true;
    }

    for (const auto &sample : projection.inspiratory_pressure) {
        if (!add_clipped_history_sample(
                projection, reader, ReportSignalId::InspiratoryPressure,
                sample)) return false;
    }
    for (const auto &sample : projection.leak) {
        if (!add_clipped_history_sample(
                projection, reader, ReportSignalId::Leak, sample)) {
            return false;
        }
    }
    return true;
}

struct AirMiniHistoryEventMetrics {
    uint32_t hypopnea = 0;
    uint32_t central_apnea = 0;
    uint32_t obstructive_apnea = 0;
    uint32_t unknown_apnea = 0;
    uint32_t rera = 0;
    uint64_t csr_duration_ms = 0;
};

void add_csr_duration(AirMiniHistoryEventMetrics &metrics,
                      int64_t start_ms,
                      int64_t end_ms) {
    if (end_ms <= start_ms) return;
    const uint64_t duration = static_cast<uint64_t>(end_ms - start_ms);
    metrics.csr_duration_ms =
        UINT64_MAX - metrics.csr_duration_ms < duration
            ? UINT64_MAX
            : metrics.csr_duration_ms + duration;
}

bool csr_boundary(const AirMiniHistoryEvent &event, int64_t &timestamp_ms) {
    timestamp_ms = event.timestamp_ms;
    if (!event.has_backdate || event.backdate_seconds == 0) return true;
    if (event.backdate_seconds > INT64_MAX / 1000) return false;
    const int64_t backdate =
        static_cast<int64_t>(event.backdate_seconds) * 1000;
    if (timestamp_ms < INT64_MIN + backdate) return false;
    timestamp_ms -= backdate;
    return true;
}

bool coverage_contains(const EdfDayCoverage &coverage, int64_t timestamp_ms);

void collect_history_event_metrics(
    const AirMiniHistoryEvents &events,
    const AirMiniHistorySessionList &sessions,
    const EdfDayCoverage *excluded_coverage,
    AirMiniHistoryEventMetrics &metrics) {
    bool csr_open = false;
    int64_t csr_start_ms = 0;
    for (const auto &event : events) {
        int64_t event_boundary_ms = event.timestamp_ms;
        if ((event.kind == AirMiniHistoryEventKind::CsrStart ||
             event.kind == AirMiniHistoryEventKind::CsrEnd) &&
            !csr_boundary(event, event_boundary_ms)) {
            continue;
        }
        if (event.kind != AirMiniHistoryEventKind::CsrStart &&
            event.kind != AirMiniHistoryEventKind::CsrEnd && excluded_coverage &&
            coverage_contains(*excluded_coverage, event_boundary_ms)) {
            continue;
        }
        switch (event.kind) {
            case AirMiniHistoryEventKind::HypopneaEnd:
                if (metrics.hypopnea != UINT32_MAX) ++metrics.hypopnea;
                break;
            case AirMiniHistoryEventKind::CentralApneaEnd:
                if (metrics.central_apnea != UINT32_MAX) {
                    ++metrics.central_apnea;
                }
                break;
            case AirMiniHistoryEventKind::ObstructiveApneaEnd:
                if (metrics.obstructive_apnea != UINT32_MAX) {
                    ++metrics.obstructive_apnea;
                }
                break;
            case AirMiniHistoryEventKind::ApneaEnd:
                if (metrics.unknown_apnea != UINT32_MAX) {
                    ++metrics.unknown_apnea;
                }
                break;
            case AirMiniHistoryEventKind::ReraEnd:
                if (metrics.rera != UINT32_MAX) ++metrics.rera;
                break;
            case AirMiniHistoryEventKind::CsrStart: {
                int64_t boundary_ms = 0;
                if (csr_boundary(event, boundary_ms)) {
                    csr_open = true;
                    csr_start_ms = boundary_ms;
                }
                break;
            }
            case AirMiniHistoryEventKind::CsrEnd: {
                int64_t boundary_ms = 0;
                if (csr_open && csr_boundary(event, boundary_ms)) {
                    for (const auto &session : sessions) {
                        int64_t cursor = std::max(csr_start_ms, session.start_ms);
                        const int64_t end = std::min(boundary_ms, session.end_ms);
                        if (excluded_coverage) {
                            for (size_t i = 0; i < excluded_coverage->count &&
                                               cursor < end; ++i) {
                                const auto &covered = excluded_coverage->intervals[i];
                                if (covered.end_ms <= cursor) continue;
                                add_csr_duration(metrics, cursor,
                                                 std::min(end, covered.start_ms));
                                cursor = std::max(cursor, covered.end_ms);
                            }
                        }
                        add_csr_duration(metrics, cursor, end);
                    }
                }
                csr_open = false;
                csr_start_ms = 0;
                break;
            }
            default:
                break;
        }
    }
}

bool has_event_coverage(const EdfDayStatisticsResult &statistics,
                        EdfInventoryFileKind kind) {
    for (const auto &coverage : statistics.event_coverage) {
        if (coverage.kind == kind && coverage.coverage.count != 0) {
            return true;
        }
    }
    return false;
}

bool coverage_contains(const EdfDayCoverage &coverage, int64_t timestamp_ms) {
    for (size_t i = 0; i < coverage.count; ++i) {
        if (timestamp_ms >= coverage.intervals[i].start_ms &&
            timestamp_ms <= coverage.intervals[i].end_ms) {
            return true;
        }
    }
    return false;
}

bool session_contains(const AirMiniHistorySessionList &sessions,
                      int64_t timestamp_ms) {
    for (const auto &session : sessions) {
        if (timestamp_ms >= session.start_ms &&
            timestamp_ms <= session.end_ms) {
            return true;
        }
    }
    return false;
}

void add_event_counts(AirMiniHistoryEventMetrics &target,
                      const AirMiniHistoryEventMetrics &source) {
    target.hypopnea = UINT32_MAX - target.hypopnea < source.hypopnea
        ? UINT32_MAX : target.hypopnea + source.hypopnea;
    target.central_apnea = UINT32_MAX - target.central_apnea <
            source.central_apnea
        ? UINT32_MAX : target.central_apnea + source.central_apnea;
    target.obstructive_apnea = UINT32_MAX - target.obstructive_apnea <
            source.obstructive_apnea
        ? UINT32_MAX : target.obstructive_apnea + source.obstructive_apnea;
    target.unknown_apnea = UINT32_MAX - target.unknown_apnea <
            source.unknown_apnea
        ? UINT32_MAX : target.unknown_apnea + source.unknown_apnea;
    target.rera = UINT32_MAX - target.rera < source.rera
        ? UINT32_MAX : target.rera + source.rera;
}

void collect_local_event_metrics(const EdfDayStatisticsResult &statistics,
                                 AirMiniHistoryEventMetrics &metrics) {
    metrics.hypopnea = statistics.event_counts[2];
    metrics.central_apnea = statistics.event_counts[3];
    metrics.obstructive_apnea = statistics.event_counts[4];
    metrics.unknown_apnea = statistics.event_counts[5];
    metrics.rera = statistics.event_counts[6];
    for (size_t i = 0; i < statistics.event_count; ++i) {
        const ReportEventRecord &event = statistics.events[i];
        if (event.code != report_event_code_value(ReportEventCode::Csr) ||
            event.duration_ms <= 0) {
            continue;
        }
        add_csr_duration(metrics, event.start_ms,
                         event.start_ms + event.duration_ms);
    }
}

bool set_str_metric(EdfStrSessionAccumulator &str,
                    const char *tag,
                    float value,
                    bool allow_replace = true) {
    if (!allow_replace) return true;
    const int index = str_signal_for_tag(tag);
    return index >= 0 && std::isfinite(value) &&
           str.set_signal_physical(static_cast<size_t>(index), value);
}

bool apply_event_metrics(const AirMiniHistoryStrProjection &projection,
                         const EdfDayStatisticsResult &statistics,
                         EdfStrSessionAccumulator &str) {
    AirMiniHistoryEventMetrics metrics;
    const bool local_eve =
        has_event_coverage(statistics, EdfInventoryFileKind::Eve);
    const bool local_csl =
        has_event_coverage(statistics, EdfInventoryFileKind::Csl);
    const EdfDayCoverage *eve_coverage = nullptr;
    const EdfDayCoverage *csl_coverage = nullptr;
    for (const auto &coverage : statistics.event_coverage) {
        if (coverage.kind == EdfInventoryFileKind::Eve) {
            eve_coverage = &coverage.coverage;
        } else if (coverage.kind == EdfInventoryFileKind::Csl) {
            csl_coverage = &coverage.coverage;
        }
    }

    const bool local_eve_covers_sessions =
        local_eve && eve_coverage &&
        coverage_covers_sessions(*eve_coverage, projection.sessions);
    const bool local_csl_covers_sessions =
        local_csl && csl_coverage &&
        coverage_covers_sessions(*csl_coverage, projection.sessions);

    if (local_eve) {
        collect_local_event_metrics(statistics, metrics);
        metrics.csr_duration_ms = 0;
    }
    const bool have_apnea_metrics = local_eve ||
                                    projection.respiratory_complete;
    const bool complete_apnea_metrics = !statistics.events_truncated &&
        (local_eve_covers_sessions || projection.respiratory_complete);
    if (projection.respiratory_complete) {
        AirMiniHistoryEventMetrics history_metrics;
        collect_history_event_metrics(projection.events, projection.sessions,
                                       eve_coverage,
                                       history_metrics);
        add_event_counts(metrics, history_metrics);
    }

    AirMiniHistoryEventMetrics csr_metrics;
    if (local_csl) {
        collect_local_event_metrics(statistics, csr_metrics);
        metrics.csr_duration_ms = csr_metrics.csr_duration_ms;
    }
    const bool have_csr_metrics = local_csl || projection.respiratory_complete;
    const bool complete_csr_metrics = !statistics.events_truncated &&
        (local_csl_covers_sessions || projection.respiratory_complete);
    if (projection.respiratory_complete) {
        AirMiniHistoryEventMetrics history_metrics;
        collect_history_event_metrics(projection.events, projection.sessions,
                                       csl_coverage,
                                       history_metrics);
        metrics.csr_duration_ms = UINT64_MAX - metrics.csr_duration_ms <
                history_metrics.csr_duration_ms
            ? UINT64_MAX
            : metrics.csr_duration_ms + history_metrics.csr_duration_ms;
    }

    if (!have_apnea_metrics && !have_csr_metrics) return true;

    uint64_t duration_ms = projection.duration_ms;
    if (duration_ms == 0) {
        const size_t duration_index =
            edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
        const int16_t duration_minutes = str.samples()[duration_index];
        if (duration_minutes > 0) {
            duration_ms = static_cast<uint64_t>(duration_minutes) * 60000ULL;
        }
    }
    if (duration_ms == 0) return true;

    const float hours = static_cast<float>(duration_ms) / 3600000.0f;
    const uint64_t apnea_count = static_cast<uint64_t>(metrics.hypopnea) +
        metrics.central_apnea + metrics.obstructive_apnea +
        metrics.unknown_apnea;
    const float hypopnea_index = metrics.hypopnea / hours;
    const float obstructive_index = metrics.obstructive_apnea / hours;
    const float central_index = metrics.central_apnea / hours;
    const float unknown_index = metrics.unknown_apnea / hours;
    const float apnea_index =
        (metrics.central_apnea + metrics.obstructive_apnea +
         metrics.unknown_apnea) / hours;
    const float ahi = apnea_count / hours;
    const float rera_index = metrics.rera / hours;

    const auto set_metric = [&](const char *tag, float value,
                                bool source_complete) {
        return set_str_metric(str, tag, value, source_complete);
    };

    if (have_apnea_metrics &&
        (!set_metric("HSC", hypopnea_index, complete_apnea_metrics) ||
         !set_metric("CSC", obstructive_index, complete_apnea_metrics) ||
         !set_metric("OSC", central_index, complete_apnea_metrics) ||
         !set_metric("USC", unknown_index, complete_apnea_metrics) ||
         !set_metric("ASC", apnea_index, complete_apnea_metrics) ||
         !set_metric("AHI", ahi, complete_apnea_metrics) ||
         !set_metric("RCC", rera_index, complete_apnea_metrics))) {
        return false;
    }
    return !have_csr_metrics ||
           set_metric("CSD",
                      static_cast<float>(metrics.csr_duration_ms) /
                          60000.0f,
                      complete_csr_metrics);
}

int seed_session_index(const AirMiniHistorySession &session,
                        const EdfStrSessionAccumulator &seed,
                        int32_t timezone_offset_minutes,
                        uint16_t &projected_end_minute,
                        uint16_t &seed_end_minute) {
    EdfLocalDateTime start;
    EdfLocalDateTime end;
    if (!edf_epoch_ms_to_local_datetime(session.start_ms,
                                        timezone_offset_minutes, start) ||
        !edf_epoch_ms_to_local_datetime(session.end_ms,
                                        timezone_offset_minutes, end)) {
        return -1;
    }
    uint16_t start_day = 0;
    uint16_t end_day = 0;
    uint16_t start_minute = 0;
    uint16_t end_minute = 0;
    if (!edf_sleep_day_epoch_days(start, start_day) ||
        !edf_sleep_day_epoch_days(end, end_day) ||
        !edf_sleep_day_minute(start, start_minute)) {
        return -1;
    }
    if (end_day == start_day) {
        if (!edf_sleep_day_minute(end, end_minute)) return -1;
    } else {
        end_minute = 1440;
    }
    if (start_day != seed.day_epoch_days()) return -1;

    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL);
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL);
    for (uint32_t i = 0; i < seed.mask_events(); ++i) {
        const int16_t seed_start = seed.samples()[on_offset + i];
        const int16_t seed_end = seed.samples()[off_offset + i];
        if (seed_start == static_cast<int16_t>(start_minute) &&
            seed_end >= seed_start && end_minute >= start_minute) {
            projected_end_minute = end_minute;
            seed_end_minute = static_cast<uint16_t>(seed_end);
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

const char *airmini_history_selector_name(AirMiniHistorySelector selector) {
    return selector_name(selector);
}

const char *airmini_history_decode_error_name(
    AirMiniHistoryDecodeError error) {
    switch (error) {
        case AirMiniHistoryDecodeError::None: return "none";
        case AirMiniHistoryDecodeError::InvalidInput: return "invalid_input";
        case AirMiniHistoryDecodeError::InvalidJson: return "invalid_json";
        case AirMiniHistoryDecodeError::UnexpectedResponse:
            return "unexpected_response";
        case AirMiniHistoryDecodeError::UnexpectedNotification:
            return "unexpected_notification";
        case AirMiniHistoryDecodeError::RequestIdMismatch:
            return "request_id_mismatch";
        case AirMiniHistoryDecodeError::StreamIdMismatch:
            return "stream_id_mismatch";
        case AirMiniHistoryDecodeError::UnsupportedSelector:
            return "unsupported_selector";
        case AirMiniHistoryDecodeError::MalformedData:
            return "malformed_data";
        case AirMiniHistoryDecodeError::CapacityExceeded:
            return "capacity_exceeded";
    }
    return "unknown";
}

const char *airmini_history_event_name(AirMiniHistoryEventKind kind) {
    return event_name(kind);
}

AirMiniHistoryData::AirMiniHistoryData(const AirMiniHistoryLimits &limits) :
    limits_(limits) {}

void AirMiniHistoryData::clear() {
    reset_logged_transfer();
    reset_settings_transfer();
    range_ = {};
    request_id_ = 0;
    stream_id_ = 0;
    active_transfer_ = ActiveTransfer::None;
}

void AirMiniHistoryData::reset_logged_transfer() {
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        requested_[i] = false;
        series_[i].state = AirMiniHistoryTransferState::NotRequested;
        series_[i].has_data = false;
        series_[i].samples.clear();
        series_[i].events.clear();
    }
    logged_active_ = false;
    if (active_transfer_ == ActiveTransfer::LoggedData) {
        active_transfer_ = ActiveTransfer::None;
    }
}

void AirMiniHistoryData::reset_settings_transfer() {
    settings_.state = AirMiniHistoryTransferState::NotRequested;
    settings_.profiles.clear();
    settings_active_ = false;
    if (active_transfer_ == ActiveTransfer::SettingsHistory) {
        active_transfer_ = ActiveTransfer::None;
    }
}

bool AirMiniHistoryData::begin(AirMiniHistoryTransferKind kind,
                               uint32_t request_id,
                               const AirMiniHistoryRange &range,
                               const AirMiniHistorySelector *selectors,
                               size_t selector_count) {
    if (!request_id || !valid_range(range)) return false;

    if (kind == AirMiniHistoryTransferKind::SettingsHistory) {
        if (selector_count != 0) return false;
        reset_settings_transfer();
        range_ = range;
        request_id_ = request_id;
        stream_id_ = 0;
        settings_active_ = true;
        active_transfer_ = ActiveTransfer::SettingsHistory;
        settings_.state = AirMiniHistoryTransferState::Pending;
        return true;
    }

    if (!selectors || selector_count == 0 ||
        selector_count > static_cast<size_t>(AirMiniHistorySelector::Count)) {
        return false;
    }

    bool selected[static_cast<size_t>(AirMiniHistorySelector::Count)] = {};
    for (size_t i = 0; i < selector_count; ++i) {
        if (!valid_selector(selectors[i])) return false;
        const size_t index = static_cast<size_t>(selectors[i]);
        if (selected[index]) return false;
        selected[index] = true;
    }

    reset_logged_transfer();
    range_ = range;
    request_id_ = request_id;
    stream_id_ = 0;
    for (size_t i = 0; i < selector_count; ++i) {
        const size_t index = static_cast<size_t>(selectors[i]);
        requested_[index] = true;
        series_[index].state = AirMiniHistoryTransferState::Pending;
    }
    logged_active_ = true;
    active_transfer_ = ActiveTransfer::LoggedData;
    return true;
}

bool AirMiniHistoryData::begin_logged_data(
    uint32_t request_id,
    const AirMiniHistorySelector *selectors,
    size_t selector_count) {
    if (settings_active_) {
        settings_active_ = false;
        if (active_transfer_ == ActiveTransfer::SettingsHistory) {
            active_transfer_ = ActiveTransfer::None;
        }
    }
    return begin(AirMiniHistoryTransferKind::LoggedData, request_id, range_,
                 selectors, selector_count);
}

bool AirMiniHistoryData::begin_logged_data(uint32_t request_id,
                                           AirMiniHistorySelector selector) {
    return begin_logged_data(request_id, &selector, 1);
}

bool AirMiniHistoryData::begin_settings_history(uint32_t request_id) {
    return begin(AirMiniHistoryTransferKind::SettingsHistory, request_id,
                 range_, nullptr, 0);
}

AirMiniHistoryDecodeResult AirMiniHistoryData::consume_response(
    const char *json,
    size_t json_len) {
    if (!json || json_len == 0 || active_transfer_ == ActiveTransfer::None) {
        return decode_result(false, false, AirMiniHistoryDecodeError::InvalidInput);
    }

    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, json, json_len)) {
        return decode_result(false, false, AirMiniHistoryDecodeError::InvalidJson);
    }
    if (!parse_id(document, request_id_)) {
        return decode_result(false, false,
                            AirMiniHistoryDecodeError::RequestIdMismatch);
    }

    const auto reject = [this](bool settings) {
        if (settings) {
            settings_.state = AirMiniHistoryTransferState::Rejected;
            settings_active_ = false;
            active_transfer_ = ActiveTransfer::None;
            return;
        }
        for (uint8_t i = 0;
             i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
            if (requested_[i]) {
                series_[i].state = AirMiniHistoryTransferState::Rejected;
            }
        }
        logged_active_ = false;
        active_transfer_ = ActiveTransfer::None;
    };
    if (document["error"].is<JsonObjectConst>()) {
        reject(active_transfer_ == ActiveTransfer::SettingsHistory);
        return decode_result(false, true,
                            AirMiniHistoryDecodeError::UnexpectedResponse);
    }

    const JsonObjectConst result = document["result"].as<JsonObjectConst>();
    if (result.isNull()) {
        reject(active_transfer_ == ActiveTransfer::SettingsHistory);
        return decode_result(false, true,
                            AirMiniHistoryDecodeError::UnexpectedResponse);
    }

    if (active_transfer_ == ActiveTransfer::SettingsHistory) {
        uint32_t stream_id = 0;
        if (!parse_uint32(result["historyStreamId"], stream_id) ||
            stream_id == 0) {
            reject(true);
            return decode_result(false, true,
                                AirMiniHistoryDecodeError::UnexpectedResponse);
        }
        stream_id_ = stream_id;
        settings_.state = AirMiniHistoryTransferState::Pending;
        settings_active_ = true;
        return decode_result(true, true, AirMiniHistoryDecodeError::None);
    }

    uint32_t stream_id = 0;
    const JsonArrayConst data_ids = result["dataIds"].as<JsonArrayConst>();
    if (!parse_uint32(result["logStreamId"], stream_id) || stream_id == 0 ||
        data_ids.isNull()) {
        reject(false);
        return decode_result(false, true,
                            AirMiniHistoryDecodeError::UnexpectedResponse);
    }

    bool changed = false;
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        if (!requested_[i]) continue;
        const char *expected = selector_name(static_cast<AirMiniHistorySelector>(i));
        bool found = false;
        for (JsonObjectConst data_id : data_ids) {
            const char *name = data_id["dataId"].as<const char *>();
            if (!name || strcmp(name, expected) != 0) continue;
            if (!data_id["valid"].is<bool>()) {
                reject(false);
                return decode_result(false, true,
                                    AirMiniHistoryDecodeError::MalformedData);
            }
            found = true;
            series_[i].state = data_id["valid"].as<bool>()
                ? AirMiniHistoryTransferState::Pending
                : AirMiniHistoryTransferState::Rejected;
            changed = true;
            break;
        }
        if (!found) {
            series_[i].state = AirMiniHistoryTransferState::Rejected;
            changed = true;
        }
    }

    stream_id_ = stream_id;
    logged_active_ = !all_logged_terminal(series_, requested_);
    if (!logged_active_) active_transfer_ = ActiveTransfer::None;
    return decode_result(true, changed, AirMiniHistoryDecodeError::None);
}

AirMiniHistoryDecodeResult AirMiniHistoryData::consume_notification(
    const char *json,
    size_t json_len) {
    if (!json || json_len == 0 || active_transfer_ == ActiveTransfer::None) {
        return decode_result(false, false, AirMiniHistoryDecodeError::InvalidInput);
    }

    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, json, json_len)) {
        return decode_result(false, false, AirMiniHistoryDecodeError::InvalidJson);
    }

    const char *method = document["method"].as<const char *>();
    const JsonObjectConst params = document["params"].as<JsonObjectConst>();
    if (!method || params.isNull()) {
        return decode_result(false, false,
                            AirMiniHistoryDecodeError::UnexpectedNotification);
    }

    uint32_t stream_id = 0;
    if (active_transfer_ == ActiveTransfer::SettingsHistory) {
        if (strcmp(method, "HistoricalEvents") != 0 ||
            !parse_uint32(params["historyStreamId"], stream_id) ||
            stream_id != stream_id_) {
            return decode_result(false, false,
                                AirMiniHistoryDecodeError::StreamIdMismatch);
        }

        const JsonObjectConst settings =
            params["Settings"].as<JsonObjectConst>();
        const JsonArrayConst profiles =
            settings["SettingProfilesSet"].as<JsonArrayConst>();
        bool complete = false;
        if (settings.isNull() || profiles.isNull() ||
            !parse_complete(params["complete"], complete)) {
            return decode_result(false, false,
                                AirMiniHistoryDecodeError::MalformedData);
        }
        for (JsonObjectConst profile : profiles) {
            if (!append_profile(settings_, range_, limits_, profile)) {
                return decode_result(false, false,
                                    AirMiniHistoryDecodeError::CapacityExceeded);
            }
        }
        if (complete) {
            settings_.state = AirMiniHistoryTransferState::Complete;
            settings_active_ = false;
            active_transfer_ = ActiveTransfer::None;
        }
        return decode_result(true, true, AirMiniHistoryDecodeError::None);
    }

    if (strcmp(method, "LoggedData") != 0 ||
        !parse_uint32(params["logStreamId"], stream_id) ||
        stream_id != stream_id_) {
        return decode_result(false, false,
                            AirMiniHistoryDecodeError::StreamIdMismatch);
    }

    const JsonArrayConst entries = params["data"].as<JsonArrayConst>();
    if (entries.isNull()) {
        return decode_result(false, false,
                            AirMiniHistoryDecodeError::MalformedData);
    }

    bool changed = false;
    for (JsonObjectConst entry : entries) {
        const char *name = entry["dataId"].as<const char *>();
        const AirMiniHistorySelector selector = selector_from_name(name);
        if (!valid_selector(selector) || !selector_requested(selector)) {
            return decode_result(false, false,
                                AirMiniHistoryDecodeError::UnsupportedSelector);
        }

        bool complete = false;
        if (!parse_complete(entry["complete"], complete)) {
            return decode_result(false, false,
                                AirMiniHistoryDecodeError::MalformedData);
        }

        bool valid = false;
        const size_t selector_index = static_cast<size_t>(selector);
        if (selector == AirMiniHistorySelector::UsageEvents ||
            selector == AirMiniHistorySelector::RespiratoryEvents) {
            const JsonArrayConst events = entry["events"].as<JsonArrayConst>();
            if (events.isNull() || !parse_logged_events(
                    series_[selector_index], range_, limits_, events)) {
                return decode_result(false, false,
                                    AirMiniHistoryDecodeError::MalformedData);
            }
            valid = true;
        } else {
            const JsonObjectConst periodic =
                entry["periodic"].as<JsonObjectConst>();
            if (periodic.isNull() || !parse_periodic(
                    series_[selector_index], range_, limits_, periodic)) {
                return decode_result(false, false,
                                    AirMiniHistoryDecodeError::MalformedData);
            }
            valid = true;
        }

        if (valid && complete) {
            series_[selector_index].state = AirMiniHistoryTransferState::Complete;
        } else if (valid) {
            series_[selector_index].state = AirMiniHistoryTransferState::Pending;
        }
        changed = true;
    }

    if (all_logged_terminal(series_, requested_)) {
        logged_active_ = false;
        active_transfer_ = ActiveTransfer::None;
    }
    return decode_result(true, changed, AirMiniHistoryDecodeError::None);
}

const AirMiniHistorySeries &AirMiniHistoryData::series(
    AirMiniHistorySelector selector) const {
    return series_or_empty(selector);
}

AirMiniHistorySeries &AirMiniHistoryData::mutable_series(
    AirMiniHistorySelector selector) {
    return series_[static_cast<size_t>(selector)];
}

const AirMiniHistorySeries &AirMiniHistoryData::series_or_empty(
    AirMiniHistorySelector selector) const {
    static const AirMiniHistorySeries empty;
    if (!valid_selector(selector)) return empty;
    return series_[static_cast<size_t>(selector)];
}

bool AirMiniHistoryData::selector_requested(
    AirMiniHistorySelector selector) const {
    return valid_selector(selector) && requested_[static_cast<size_t>(selector)];
}

uint32_t AirMiniHistoryData::rejected_mask() const {
    uint32_t mask = 0;
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        if (series_[i].state == AirMiniHistoryTransferState::Rejected) {
            mask |= 1u << i;
        }
    }
    return mask;
}

void AirMiniHistoryData::set_window(int64_t from_ms, int64_t to_ms) {
    range_ = {from_ms, to_ms};
}

bool AirMiniHistoryData::merge_from(const AirMiniHistoryData &incoming) {
    try {
        for (uint8_t i = 0;
             i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
            AirMiniHistorySeries &target = series_[i];
            const AirMiniHistorySeries &source = incoming.series_[i];
            AirMiniHistorySamples samples;
            samples.reserve(std::min(limits_.samples_per_selector,
                target.samples.size() + source.samples.size()));
            size_t left = 0;
            size_t right = 0;
            while (left < target.samples.size() || right < source.samples.size()) {
                if (right == source.samples.size() ||
                    (left < target.samples.size() &&
                     target.samples[left].timestamp_ms < source.samples[right].timestamp_ms)) {
                    samples.push_back(target.samples[left++]);
                } else {
                    if (left < target.samples.size() &&
                        target.samples[left].timestamp_ms == source.samples[right].timestamp_ms) {
                        ++left;
                    }
                    samples.push_back(source.samples[right++]);
                }
                if (samples.size() > limits_.samples_per_selector) return false;
            }
            target.samples.swap(samples);

            if (!merge_retained_rows(target.events, source.events, limits_.events,
                [](const AirMiniHistoryEvent &event) { return event.timestamp_ms; },
                [](const AirMiniHistoryEvent &a, const AirMiniHistoryEvent &b) {
                    return a.kind == b.kind && strcmp(a.name, b.name) == 0 &&
                        a.has_duration == b.has_duration &&
                        a.duration_seconds == b.duration_seconds &&
                        a.has_backdate == b.has_backdate &&
                        a.backdate_seconds == b.backdate_seconds;
                })) return false;

            target.has_data |= source.has_data;
            merge_state(target.state, source.state);
        }

        if (!merge_retained_rows(settings_.profiles, incoming.settings_.profiles,
            limits_.settings_profiles,
            [](const AirMiniHistorySettingProfile &profile) { return profile.applied_time_ms; },
            [](const AirMiniHistorySettingProfile &a, const AirMiniHistorySettingProfile &b) {
                return a.json == b.json;
            })) return false;

        merge_state(settings_.state, incoming.settings_.state);
        if (range_.from_ms == 0 && range_.to_ms == 0) range_ = incoming.range_;
    } catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

bool AirMiniHistoryData::copy_window_from(const AirMiniHistoryData &incoming,
                                          int64_t from_ms,
                                          int64_t to_ms) {
    const AirMiniHistoryRange range = {from_ms, to_ms};
    if (!valid_range(range)) return false;

    AirMiniHistoryData copy(limits_);
    copy.range_ = range;
    copy.settings_.state = incoming.settings_.state;
    try {
        for (uint8_t i = 0;
             i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
            const auto &source = incoming.series_[i];
            auto &target = copy.series_[i];
            target.state = source.state;
            const auto first_sample = std::lower_bound(
                source.samples.begin(), source.samples.end(), from_ms,
                [](const AirMiniHistorySample &sample, int64_t time) {
                    return sample.timestamp_ms < time;
                });
            for (auto it = first_sample;
                 it != source.samples.end() && it->timestamp_ms < to_ms; ++it) {
                if (target.samples.size() >= limits_.samples_per_selector) return false;
                target.samples.push_back(*it);
            }

            auto first_event = std::lower_bound(
                source.events.begin(), source.events.end(), from_ms,
                [](const AirMiniHistoryEvent &event, int64_t time) {
                    return event.timestamp_ms < time;
                });
            if (i == static_cast<uint8_t>(AirMiniHistorySelector::UsageEvents)) {
                // Carry only an open interval over the noon boundary.
                auto context = first_event;
                while (context != source.events.begin()) {
                    --context;
                    if (!event_is_mask_context(context->kind)) continue;
                    if (context->kind == AirMiniHistoryEventKind::MaskOn) {
                        target.events.push_back(*context);
                    }
                    break;
                }
            }
            for (auto it = first_event;
                 it != source.events.end() && it->timestamp_ms < to_ms; ++it) {
                if (target.events.size() >= limits_.events) return false;
                target.events.push_back(*it);
            }
            target.has_data = !target.samples.empty() || !target.events.empty();
        }

        const auto &profiles = incoming.settings_.profiles;
        auto first_profile = std::lower_bound(profiles.begin(), profiles.end(), from_ms,
            [](const AirMiniHistorySettingProfile &profile, int64_t time) {
                return profile.applied_time_ms < time;
            });
        if (first_profile != profiles.begin()) {
            --first_profile;
            const int64_t time = first_profile->applied_time_ms;
            while (first_profile != profiles.begin() &&
                   (first_profile - 1)->applied_time_ms == time) --first_profile;
        }
        for (auto it = first_profile;
             it != profiles.end() && it->applied_time_ms < to_ms; ++it) {
            if (copy.settings_.profiles.size() >= limits_.settings_profiles) return false;
            copy.settings_.profiles.push_back(*it);
        }
    } catch (const std::bad_alloc &) {
        return false;
    }
    *this = std::move(copy);
    return true;
}

bool AirMiniHistoryData::serialize(LargeTextBuffer &out) const {
    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    document["version"] = HISTORY_SCHEMA;
    document["range"]["from"] = range_.from_ms;
    document["range"]["to"] = range_.to_ms;

    JsonArray logged = document["logged"].to<JsonArray>();
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        const auto selector = static_cast<AirMiniHistorySelector>(i);
        const AirMiniHistorySeries &source = series_[i];
        JsonObject entry = logged.add<JsonObject>();
        entry["dataId"] = selector_name(selector);
        entry["state"] = static_cast<uint8_t>(source.state);
        entry["hasData"] = source.has_data;
        JsonArray samples = entry["samples"].to<JsonArray>();
        for (const auto &sample : source.samples) {
            JsonObject value = samples.add<JsonObject>();
            value["time"] = sample.timestamp_ms;
            value["value"] = sample.value;
        }
        JsonArray events = entry["events"].to<JsonArray>();
        for (const auto &event : source.events) {
            JsonObject value = events.add<JsonObject>();
            value["time"] = event.timestamp_ms;
            value["event"] = event.name;
            if (event.has_duration) value["durationSeconds"] = event.duration_seconds;
            if (event.has_backdate) value["backdateSeconds"] = event.backdate_seconds;
        }
    }

    JsonObject settings = document["settings"].to<JsonObject>();
    settings["state"] = static_cast<uint8_t>(settings_.state);
    JsonArray profiles = settings["profiles"].to<JsonArray>();
    for (const auto &profile : settings_.profiles) {
        JsonObject value = profiles.add<JsonObject>();
        value["appliedTimeMs"] = profile.applied_time_ms;
        value["json"] = profile.json.c_str();
    }

    if (document.overflowed()) return false;

    LargeTextBuffer generated;
    JsonTextWriter<LargeTextBuffer> writer(generated);
    if (serializeJson(document, writer) == 0 || document.overflowed() ||
        generated.overflowed()) {
        return false;
    }
    out.swap(generated);
    return true;
}

bool AirMiniHistoryData::deserialize(const char *json, size_t json_len) {
    if (!json || json_len == 0) return false;

    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, json, json_len)) return false;
    const JsonObjectConst root = document.as<JsonObjectConst>();
    if (root.isNull() || (root["version"] | 0u) != HISTORY_SCHEMA) return false;

    AirMiniHistoryData decoded(limits_);
    const JsonObjectConst range = root["range"].as<JsonObjectConst>();
    if (!range.isNull()) {
        if (!parse_int64(range["from"], decoded.range_.from_ms) ||
            !parse_int64(range["to"], decoded.range_.to_ms) ||
            !valid_range(decoded.range_)) {
            return false;
        }
    }

    const JsonArrayConst logged = root["logged"].as<JsonArrayConst>();
    if (logged.isNull()) return false;
    for (JsonObjectConst entry : logged) {
        const AirMiniHistorySelector selector =
            selector_from_name(entry["dataId"].as<const char *>());
        if (!valid_selector(selector)) return false;
        const uint32_t state = entry["state"] | 0u;
        if (state > static_cast<uint32_t>(AirMiniHistoryTransferState::Complete)) {
            return false;
        }
        AirMiniHistorySeries &series = decoded.series_[static_cast<size_t>(selector)];
        series.state = static_cast<AirMiniHistoryTransferState>(state);
        decoded.requested_[static_cast<size_t>(selector)] = state != 0;
        series.has_data = entry["hasData"] | false;

        const JsonArrayConst samples = entry["samples"].as<JsonArrayConst>();
        if (!samples.isNull()) {
            for (JsonObjectConst value : samples) {
                AirMiniHistorySample sample;
                if (!parse_int64(value["time"], sample.timestamp_ms) ||
                    !(value["value"].is<float>() ||
                      value["value"].is<double>())) {
                    return false;
                }
                sample.value = value["value"].as<float>();
                if (!append_sample(decoded.series_[static_cast<size_t>(selector)],
                                   {}, decoded.limits_,
                                   sample.timestamp_ms,
                                   sample.value)) {
                    return false;
                }
            }
        }
        const JsonArrayConst events = entry["events"].as<JsonArrayConst>();
        if (!events.isNull()) {
            for (JsonObjectConst value : events) {
                AirMiniHistoryEvent event;
                if (!parse_int64(value["time"], event.timestamp_ms) ||
                    !copy_name(event.name, sizeof(event.name),
                               value["event"].as<const char *>())) {
                    return false;
                }
                event.kind = event_kind(event.name);
                if (!value["durationSeconds"].isNull()) {
                    if (!parse_uint32(value["durationSeconds"],
                                      event.duration_seconds)) return false;
                    event.has_duration = true;
                }
                if (!value["backdateSeconds"].isNull()) {
                    if (!parse_uint32(value["backdateSeconds"],
                                      event.backdate_seconds)) return false;
                    event.has_backdate = true;
                }
                if (!append_event(decoded.series_[static_cast<size_t>(selector)],
                                  {}, decoded.limits_, event)) {
                    return false;
                }
            }
        }
    }

    const JsonObjectConst settings = root["settings"].as<JsonObjectConst>();
    if (!settings.isNull()) {
        const uint32_t state = settings["state"] | 0u;
        if (state > static_cast<uint32_t>(AirMiniHistoryTransferState::Complete)) {
            return false;
        }
        decoded.settings_.state = static_cast<AirMiniHistoryTransferState>(state);
        const JsonArrayConst profiles = settings["profiles"].as<JsonArrayConst>();
        if (!profiles.isNull()) {
            for (JsonObjectConst value : profiles) {
                int64_t applied = 0;
                const char *profile_json = value["json"].as<const char *>();
                if (!parse_int64(value["appliedTimeMs"], applied) ||
                    !profile_json ||
                    strlen(profile_json) > limits_.settings_json_bytes ||
                    decoded.settings_.profiles.size() >= limits_.settings_profiles) {
                    return false;
                }
                AirMiniHistorySettingProfile profile;
                profile.applied_time_ms = applied;
                profile.json = profile_json;
                decoded.settings_.profiles.push_back(std::move(profile));
            }
        }
    }

    std::stable_sort(decoded.settings_.profiles.begin(),
                     decoded.settings_.profiles.end(), profile_less);

    std::swap(range_, decoded.range_);
    std::swap(request_id_, decoded.request_id_);
    std::swap(stream_id_, decoded.stream_id_);
    std::swap(logged_active_, decoded.logged_active_);
    std::swap(settings_active_, decoded.settings_active_);
    std::swap(active_transfer_, decoded.active_transfer_);
    for (uint8_t i = 0;
         i < static_cast<uint8_t>(AirMiniHistorySelector::Count); ++i) {
        std::swap(requested_[i], decoded.requested_[i]);
        std::swap(series_[i].state, decoded.series_[i].state);
        std::swap(series_[i].has_data, decoded.series_[i].has_data);
        series_[i].samples.swap(decoded.series_[i].samples);
        series_[i].events.swap(decoded.series_[i].events);
    }
    std::swap(settings_.state, decoded.settings_.state);
    settings_.profiles.swap(decoded.settings_.profiles);
    return true;
}

void AirMiniHistoryStrProjection::clear() {
    usage_complete = false;
    respiratory_complete = false;
    pressure_complete = false;
    leak_complete = false;
    settings_complete = false;
    used_local_sessions = false;
    has_unclosed_history = false;
    duration_ms = 0;
    mask_event_count = 0;
    settings_profile_index = -1;
    sessions.clear();
    mask_sessions.clear();
    events.clear();
    inspiratory_pressure.clear();
    leak.clear();
}

bool AirMiniHistoryData::project_day(
    const AirMiniHistoryProjectionInput &input,
    AirMiniHistoryStrProjection &out) const {
    if (input.day_start_ms >= input.day_end_ms) return false;
    out.clear();

    const AirMiniHistorySeries &usage = series(AirMiniHistorySelector::UsageEvents);
    const AirMiniHistorySeries &resp = series(AirMiniHistorySelector::RespiratoryEvents);
    const AirMiniHistorySeries &pressure =
        series(AirMiniHistorySelector::InspiratoryPressure);
    const AirMiniHistorySeries &leak = series(AirMiniHistorySelector::Leak);
    out.usage_complete = usage.state == AirMiniHistoryTransferState::Complete;
    out.respiratory_complete = resp.state == AirMiniHistoryTransferState::Complete;
    out.pressure_complete =
        pressure.state == AirMiniHistoryTransferState::Complete;
    out.leak_complete = leak.state == AirMiniHistoryTransferState::Complete;
    out.settings_complete =
        settings_.state == AirMiniHistoryTransferState::Complete;

    int64_t observed_end_ms = input.observed_end_ms;
    if (observed_end_ms == 0) {
        if (!latest_mapped_history_time(series_, input, observed_end_ms)) {
            observed_end_ms = input.day_start_ms;
        }
    }
    observed_end_ms = std::min(observed_end_ms, input.day_end_ms);

    AirMiniHistoryStrProjection historical;
    if (!collect_history_sessions(
            series_, static_cast<size_t>(AirMiniHistorySelector::Count), input,
            observed_end_ms, historical)) {
        return false;
    }

    try {
        out.mask_sessions.reserve(historical.sessions.size());
        for (const auto &session : historical.sessions) {
            out.mask_sessions.push_back(session);
        }
    } catch (const std::bad_alloc &) {
        return false;
    }

    const bool have_seed = input.local_str_has_session && input.local_str;
    if (input.local_sessions && input.local_session_count) {
        out.used_local_sessions = true;
        try {
            for (size_t i = 0; i < input.local_session_count; ++i) {
                const auto &session = input.local_sessions[i];
                if (session.end_ms <= session.start_ms) continue;
                if (!add_session(out, session.start_ms, session.end_ms,
                                 session.closed_at_observed_end,
                                 input.day_start_ms, input.day_end_ms)) {
                    return false;
                }
            }
            if (have_seed) {
                const size_t duration_index =
                    edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
                if (input.local_str->samples()[duration_index] > 0) {
                    out.duration_ms = static_cast<uint64_t>(
                        input.local_str->samples()[duration_index]) * 60000ULL;
                }
                out.mask_event_count = input.local_str->mask_events();
            }
            for (const auto &candidate : historical.sessions) {
                if (!add_session(out, candidate.start_ms, candidate.end_ms,
                                 candidate.closed_at_observed_end,
                                 input.day_start_ms, input.day_end_ms)) {
                    return false;
                }
            }
        } catch (const std::bad_alloc &) {
            return false;
        }
    } else if (have_seed) {
        // Reconstruct the seed intervals so a later historical session can be
        // added without counting the recorder copy twice.
        out.used_local_sessions = true;
        const size_t duration_index =
            edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
        const uint32_t seed_mask_events = input.local_str->mask_events();
        const size_t mask_on_offset =
            edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL);
        const size_t mask_off_offset =
            edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL);
        for (uint32_t i = 0; i < seed_mask_events; ++i) {
            const int16_t on = input.local_str->samples()[mask_on_offset + i];
            const int16_t off = input.local_str->samples()[mask_off_offset + i];
            if (on < 0 || off < on) continue;
            if (!add_session(out,
                             input.day_start_ms + static_cast<int64_t>(on) * 60000,
                             input.day_start_ms + static_cast<int64_t>(off) * 60000,
                             false, input.day_start_ms, input.day_end_ms)) {
                return false;
            }
        }
        out.duration_ms = input.local_str->samples()[duration_index] > 0
            ? static_cast<uint64_t>(input.local_str->samples()[duration_index]) *
                  60000ULL
            : 0;
        out.mask_event_count = seed_mask_events;
        for (const auto &candidate : historical.sessions) {
            if (!add_session(out, candidate.start_ms, candidate.end_ms,
                             candidate.closed_at_observed_end,
                             input.day_start_ms, input.day_end_ms)) {
                return false;
            }
        }
    } else {
        out.sessions.swap(historical.sessions);
        out.duration_ms = historical.duration_ms;
        out.mask_event_count = historical.mask_event_count;
        out.has_unclosed_history = historical.has_unclosed_history;
    }

    if (have_seed) {
        if (input.local_sessions && input.local_session_count) {
            out.mask_event_count = input.local_str->mask_events();
        }
        const size_t duration_index =
            edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
        const int16_t seed_duration_minutes =
            input.local_str->samples()[duration_index];
        if (seed_duration_minutes > 0) {
            uint64_t preserved_duration_ms =
                static_cast<uint64_t>(seed_duration_minutes) * 60000ULL;
            for (const auto &session : out.sessions) {
                uint16_t projected_end_minute = 0;
                uint16_t seed_end_minute = 0;
                const int seed_index = seed_session_index(
                    session, *input.local_str, input.timezone_offset_minutes,
                    projected_end_minute, seed_end_minute);
                if (seed_index >= 0) {
                    if (projected_end_minute > seed_end_minute) {
                        preserved_duration_ms += static_cast<uint64_t>(
                            projected_end_minute - seed_end_minute) * 60000ULL;
                    }
                } else {
                    preserved_duration_ms += static_cast<uint64_t>(
                        session.end_ms - session.start_ms);
                }
            }
            out.duration_ms = preserved_duration_ms;
        }
    }

    for (const auto &event : resp.events) {
        int64_t timestamp_ms = 0;
        if (!history_time_to_utc(event.timestamp_ms, input, timestamp_ms)) {
            continue;
        }
        if (timestamp_ms >= input.day_start_ms &&
            timestamp_ms < input.day_end_ms) {
            if (!session_contains(out.sessions, timestamp_ms)) continue;
            try {
                AirMiniHistoryEvent mapped = event;
                mapped.timestamp_ms = timestamp_ms;
                out.events.push_back(mapped);
            } catch (const std::bad_alloc &) {
                return false;
            }
        }
    }

    for (const auto &sample : pressure.samples) {
        int64_t timestamp_ms = 0;
        if (!history_time_to_utc(sample.timestamp_ms, input, timestamp_ms)) {
            continue;
        }
        if (timestamp_ms >= input.day_start_ms &&
            timestamp_ms < input.day_end_ms) {
            try {
                AirMiniHistorySample mapped = sample;
                mapped.timestamp_ms = timestamp_ms;
                out.inspiratory_pressure.push_back(mapped);
            } catch (const std::bad_alloc &) {
                return false;
            }
        }
    }
    for (const auto &sample : leak.samples) {
        int64_t timestamp_ms = 0;
        if (!history_time_to_utc(sample.timestamp_ms, input, timestamp_ms)) {
            continue;
        }
        if (timestamp_ms >= input.day_start_ms &&
            timestamp_ms < input.day_end_ms) {
            try {
                AirMiniHistorySample mapped = sample;
                mapped.timestamp_ms = timestamp_ms;
                out.leak.push_back(mapped);
            } catch (const std::bad_alloc &) {
                return false;
            }
        }
    }

    int64_t latest_session_start = 0;
    for (const auto &session : out.sessions) {
        latest_session_start = std::max(latest_session_start, session.start_ms);
    }

    int64_t raw_session_start = latest_session_start;
    bool raw_start_known = false;
    if (input.local_statistics) {
        const auto &local = *input.local_statistics;
        for (size_t i = 0; i < local.provenance_session_count; ++i) {
            const auto &session = local.provenance_sessions[i];
            if (std::max(session.canonical_therapy_start_ms,
                         input.day_start_ms) == latest_session_start) {
                raw_session_start = session.raw_therapy_start_ms;
                raw_start_known = true;
                break;
            }
        }
    }
    if (!raw_start_known) {
        for (const auto &event : usage.events) {
            int64_t canonical_start = 0;
            if (event.kind == AirMiniHistoryEventKind::MaskOn &&
                history_time_to_utc(event.timestamp_ms, input, canonical_start) &&
                std::max(canonical_start, input.day_start_ms) == latest_session_start) {
                raw_session_start = event.timestamp_ms;
                raw_start_known = true;
            }
        }
    }
    if (!raw_start_known && input.clock.externally_referenced &&
        latest_session_start >= input.clock_window_start_ms &&
        latest_session_start < input.clock_window_end_ms) {
        raw_session_start += input.clock.device_minus_utc_ms;
    }

    size_t profile_index = SIZE_MAX;
    for (size_t i = 0; latest_session_start != 0 &&
                       i < settings_.profiles.size(); ++i) {
        if (settings_.profiles[i].applied_time_ms <= raw_session_start) {
            profile_index = i;
        }
    }
    if (profile_index != SIZE_MAX) {
        out.settings_profile_index = static_cast<int>(profile_index);
    }
    return true;
}

bool airmini_history_prepare_str(
    const AirMiniHistoryData &history,
    const AirMiniHistoryProjectionInput &input,
    EdfDayStatisticsReader &local_statistics,
    AirMiniHistoryStrProjection &projection,
    const char *&error) {
    error = nullptr;
    if (local_statistics.status().active()) {
        error = "edf_statistics_pending";
        return false;
    }
    if (local_statistics.status().state != EdfDayStatisticsState::Complete &&
        local_statistics.status().state != EdfDayStatisticsState::Finalize) {
        error = "edf_statistics_unavailable";
        return false;
    }

    AirMiniHistoryProjectionInput effective = input;
    if (local_statistics.status().state == EdfDayStatisticsState::Complete) {
        effective.local_statistics = &local_statistics.result();
    }

    AirMiniHistorySession statistics_sessions[
        AC_EDF_DAY_STATISTICS_SESSION_MAX] = {};
    if (effective.local_session_count == 0 &&
        local_statistics.status().state == EdfDayStatisticsState::Complete) {
        const EdfDayStatisticsResult &statistics = local_statistics.result();
        for (size_t i = 0;
             i < statistics.provenance_session_count &&
                 i < AC_EDF_DAY_STATISTICS_SESSION_MAX;
             ++i) {
            const EdfSessionMetadata &metadata =
                statistics.provenance_sessions[i];
            if (metadata.canonical_therapy_start_ms <= 0 ||
                metadata.canonical_therapy_end_ms <=
                    metadata.canonical_therapy_start_ms) {
                continue;
            }
            statistics_sessions[effective.local_session_count++] = {
                metadata.canonical_therapy_start_ms,
                metadata.canonical_therapy_end_ms,
                !metadata.finalized,
            };
        }
        if (effective.local_session_count != 0) {
            effective.local_sessions = statistics_sessions;
        }
    }

    if (!history.project_day(effective, projection)) {
        error = "history_projection_failed";
        return false;
    }

    if (!add_history_fallback(projection, local_statistics)) {
        error = "history_fallback_failed";
        return false;
    }
    return true;
}

bool airmini_history_apply_str(const AirMiniHistoryData &history,
                               const AirMiniHistoryProjectionInput &input,
                               EdfDayStatisticsReader &local_statistics,
                               const AirMiniHistoryStrProjection &projection,
                               EdfStrSessionAccumulator &str,
                               const char *&error) {
    error = nullptr;
    if (local_statistics.status().active()) {
        error = "edf_statistics_pending";
        return false;
    }
    if (local_statistics.status().state != EdfDayStatisticsState::Complete) {
        error = "edf_statistics_unavailable";
        return false;
    }

    const bool had_seed = input.local_str_has_session && input.local_str;
    if (had_seed) {
        str = *input.local_str;
    } else {
        str = EdfStrSessionAccumulator{};
    }

    if (!str.active() && !projection.sessions.empty()) {
        EdfLocalDateTime start;
        if (!edf_epoch_ms_to_local_datetime(projection.sessions.front().start_ms,
                                            input.timezone_offset_minutes, start)) {
            error = "history_session_time_invalid";
            return false;
        }
        uint16_t day_epoch = 0;
        if (!edf_sleep_day_epoch_days(start, day_epoch)) {
            error = "history_sleep_day_invalid";
            return false;
        }
        EdfLocalDateTime sleep_day_start;
        if (!edf_sleep_day_start(start, sleep_day_start)) {
            error = "history_sleep_day_invalid";
            return false;
        }
        str.reset_day(day_epoch, sleep_day_start);
    }

    if (!had_seed) {
        for (const auto &session : projection.sessions) {
            if (!apply_session_to_str(session, input.timezone_offset_minutes,
                                       str, StrIntervalKind::Therapy)) {
                error = "history_str_session_failed";
                return false;
            }
        }
    }

    const AirMiniHistorySessionList &sessions_to_apply =
        !had_seed && projection.mask_sessions.empty()
            ? projection.sessions : projection.mask_sessions;
    for (const auto &session : sessions_to_apply) {
        if (had_seed) {
            uint16_t projected_end_minute = 0;
            uint16_t seed_end_minute = 0;
            const int seed_index = seed_session_index(
                session, *input.local_str, input.timezone_offset_minutes,
                projected_end_minute, seed_end_minute);
            if (seed_index >= 0) {
                if (projected_end_minute <= seed_end_minute) continue;

                EdfLocalDateTime end;
                if (!edf_epoch_ms_to_local_datetime(
                        session.end_ms, input.timezone_offset_minutes, end)) {
                    error = "history_session_time_invalid";
                    return false;
                }
                EdfStrSessionStatus status = EdfStrSessionStatus::Ok;
                if (!str.extend_mask_event(static_cast<uint8_t>(seed_index),
                                           end, status)) {
                    error = "history_str_session_extend_failed";
                    return false;
                }
                continue;
            }
        }
        if (!apply_session_to_str(session, input.timezone_offset_minutes, str,
                had_seed ? StrIntervalKind::TherapyAndMask
                         : StrIntervalKind::Mask)) {
            error = "history_str_session_failed";
            return false;
        }
    }

    const EdfDayStatisticsResult &statistics = local_statistics.result();
    for (size_t i = 0; i < statistics.signal_count; ++i) {
        const EdfDaySignalStatistics &signal = statistics.signals[i];
        if (!signal.valid) continue;
        if (!signal.primary && std::any_of(
                statistics.signals, statistics.signals + statistics.signal_count,
                [&](const EdfDaySignalStatistics &candidate) {
                    return candidate.valid && candidate.primary &&
                           candidate.signal == signal.signal;
                })) continue;

        const float canonical_to_str_divisor =
            ((signal.signal == ReportSignalId::Flow &&
              signal.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
             (signal.signal == ReportSignalId::Leak &&
              signal.source == ReportSourceId::Leak0p5Hz))
                ? 60.0f : 1.0f;
        switch (signal.signal) {
            case ReportSignalId::InspiratoryPressure:
                apply_stat_value(signal, signal.p50_milli,
                                 "PIM", 1.0f, str);
                apply_stat_value(signal, signal.p95_milli,
                                 "PI9", 1.0f, str);
                apply_stat_value(signal, signal.max_milli,
                                 "PIA", 1.0f, str);
                break;
            case ReportSignalId::ExpiratoryPressure:
                apply_stat_value(signal, signal.p50_milli,
                                 "PEM", 1.0f, str);
                apply_stat_value(signal, signal.p95_milli,
                                 "PE9", 1.0f, str);
                apply_stat_value(signal, signal.max_milli,
                                 "PEA", 1.0f, str);
                break;
            case ReportSignalId::MaskPressure:
                apply_stat_value(signal, signal.p50_milli,
                                 "MSP", 1.0f, str);
                apply_stat_value(signal, signal.p95_milli,
                                 "PM9", 1.0f, str);
                apply_stat_value(signal, signal.max_milli,
                                 "PMA", 1.0f, str);
                break;
            case ReportSignalId::Flow:
                apply_stat_value(signal, signal.p5_milli,
                                 "RFM", canonical_to_str_divisor,
                                 str);
                apply_stat_value(signal, signal.p95_milli,
                                 "R95", canonical_to_str_divisor,
                                 str);
                break;
            case ReportSignalId::Leak:
                apply_stat_value(signal, signal.p50_milli,
                                 "LKM", canonical_to_str_divisor,
                                 str);
                apply_stat_value(signal, signal.p95_milli,
                                 "LK9", canonical_to_str_divisor,
                                 str);
                apply_stat_value(signal, signal.p70_milli,
                                 "LK7", canonical_to_str_divisor,
                                 str);
                apply_stat_value(signal, signal.max_milli,
                                 "LMX", canonical_to_str_divisor,
                                 str);
                break;
            case ReportSignalId::SpO2:
                apply_stat_value(signal, signal.p50_milli,
                                 "SOM", 1.0f, str);
                apply_stat_value(signal, signal.p95_milli,
                                 "SO9", 1.0f, str);
                apply_stat_value(signal, signal.max_milli,
                                 "SOX", 1.0f, str);
                break;
            case ReportSignalId::Pulse:
                // The current STR layout has no heart-rate field.
                break;
            default:
                // AirMini does not provide trustworthy RR/VT/MV fields.
                break;
        }
    }

    if (!apply_event_metrics(projection, statistics, str)) {
        error = "history_event_metrics_failed";
        return false;
    }

    if (projection.settings_profile_index >= 0 &&
        projection.settings_profile_index <
            static_cast<int>(history.settings().profiles.size())) {
        const AirMiniHistorySettingProfile &profile =
            history.settings().profiles[
                static_cast<size_t>(projection.settings_profile_index)];
        EdfStrSettingsApplyResult applied;
        if (!edf_str_apply_airmini_settings_profile(
                RpcPayloadView(profile.json.c_str(), profile.json.size()),
                str, applied)) {
            error = applied.error ? applied.error : "history_settings_failed";
            return false;
        }
    }

    return true;
}

}  // namespace aircannect
