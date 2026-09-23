#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "as11_clock.h"
#include "edf_str_session.h"
#include "large_allocator.h"
#include "large_text_buffer.h"
#include "sleep_day_id.h"

namespace aircannect {

class EdfDayStatisticsReader;
struct EdfDayStatisticsResult;

enum class AirMiniHistorySelector : uint8_t {
    UsageEvents,
    RespiratoryEvents,
    InspiratoryPressure,
    Leak,
    Count,
};

enum class AirMiniHistoryTransferKind : uint8_t {
    LoggedData,
    SettingsHistory,
};

enum class AirMiniHistoryTransferState : uint8_t {
    NotRequested,
    Pending,
    Rejected,
    Complete,
};

enum class AirMiniHistoryDecodeError : uint8_t {
    None,
    InvalidInput,
    InvalidJson,
    UnexpectedResponse,
    UnexpectedNotification,
    RequestIdMismatch,
    StreamIdMismatch,
    UnsupportedSelector,
    MalformedData,
    CapacityExceeded,
};

enum class AirMiniHistoryEventKind : uint8_t {
    Unknown,
    MaskOn,
    MaskOff,
    PowerOff,
    HypopneaEnd,
    CentralApneaEnd,
    ObstructiveApneaEnd,
    ApneaEnd,
    ReraEnd,
    CsrStart,
    CsrEnd,
};

static constexpr size_t AC_AIRMINI_HISTORY_EVENT_NAME_MAX = 32;

struct AirMiniHistoryLimits {
    size_t samples_per_selector = 8192;
    size_t events = 4096;
    size_t settings_profiles = 365;
    size_t settings_json_bytes = 8192;
};

struct AirMiniHistorySample {
    int64_t timestamp_ms = 0;
    float value = 0.0f;
};

struct AirMiniHistoryEvent {
    int64_t timestamp_ms = 0;
    AirMiniHistoryEventKind kind = AirMiniHistoryEventKind::Unknown;
    uint32_t duration_seconds = 0;
    bool has_duration = false;
    uint32_t backdate_seconds = 0;
    bool has_backdate = false;
    char name[AC_AIRMINI_HISTORY_EVENT_NAME_MAX] = {};
};

using AirMiniHistorySamples =
    std::vector<AirMiniHistorySample, LargeAllocator<AirMiniHistorySample>>;
using AirMiniHistoryEvents =
    std::vector<AirMiniHistoryEvent, LargeAllocator<AirMiniHistoryEvent>>;
using AirMiniHistoryText =
    std::basic_string<char, std::char_traits<char>, LargeAllocator<char>>;

struct AirMiniHistorySettingProfile {
    int64_t applied_time_ms = 0;
    AirMiniHistoryText json;
};

using AirMiniHistorySettingProfiles = std::vector<
    AirMiniHistorySettingProfile,
    LargeAllocator<AirMiniHistorySettingProfile>>;

struct AirMiniHistorySeries {
    AirMiniHistoryTransferState state =
        AirMiniHistoryTransferState::NotRequested;
    bool has_data = false;
    AirMiniHistorySamples samples;
    AirMiniHistoryEvents events;
};

struct AirMiniHistorySettings {
    AirMiniHistoryTransferState state =
        AirMiniHistoryTransferState::NotRequested;
    AirMiniHistorySettingProfiles profiles;
};

struct AirMiniHistoryDecodeResult {
    bool ok = false;
    bool state_changed = false;
    AirMiniHistoryDecodeError error = AirMiniHistoryDecodeError::None;
};

struct AirMiniHistoryRange {
    int64_t from_ms = 0;
    int64_t to_ms = 0;
};

struct AirMiniHistorySession;

struct AirMiniHistoryProjectionInput {
    SleepDayId day;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    int32_t timezone_offset_minutes = 0;
    As11ClockTransform clock;
    const EdfDayStatisticsResult *local_statistics = nullptr;

    // Canonical UTC bounds for the only window to which the sampled clock
    // transform may be applied. Zero bounds disable this fallback.
    int64_t clock_window_start_ms = 0;
    int64_t clock_window_end_ms = 0;

    // Explicit end of data observed by the caller. It is never replaced with
    // the current time. Zero means derive the bound from retained records.
    int64_t observed_end_ms = 0;

    const AirMiniHistorySession *local_sessions = nullptr;
    size_t local_session_count = 0;

    // A local STR already contains authoritative duration and mask metadata.
    // Local and historical usage intervals are unioned; seed sessions are not
    // applied a second time when the resulting STR is assembled.
    const EdfStrSessionAccumulator *local_str = nullptr;
    bool local_str_has_session = false;
};

struct AirMiniHistorySession {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    bool closed_at_observed_end = false;
};

using AirMiniHistorySessionList =
    std::vector<AirMiniHistorySession, LargeAllocator<AirMiniHistorySession>>;

struct AirMiniHistoryStrProjection {
    bool usage_complete = false;
    bool respiratory_complete = false;
    bool pressure_complete = false;
    bool leak_complete = false;
    bool settings_complete = false;
    bool used_local_sessions = false;
    bool has_unclosed_history = false;
    uint64_t duration_ms = 0;
    uint16_t duration_minutes = 0;
    uint32_t mask_event_count = 0;
    int settings_profile_index = -1;

    AirMiniHistorySessionList sessions;
    // Explicit MaskOn/MaskOff intervals from AirMini history. These are kept
    // separate from therapy spans reported by local EDF metadata.
    AirMiniHistorySessionList mask_sessions;
    AirMiniHistoryEvents events;
    AirMiniHistorySamples inspiratory_pressure;
    AirMiniHistorySamples leak;

    void clear();
};

const char *airmini_history_selector_name(AirMiniHistorySelector selector);
const char *airmini_history_decode_error_name(
    AirMiniHistoryDecodeError error);
const char *airmini_history_event_name(AirMiniHistoryEventKind kind);

class AirMiniHistoryData {
public:
    explicit AirMiniHistoryData(const AirMiniHistoryLimits &limits = {});

    AirMiniHistoryData(const AirMiniHistoryData &) = delete;
    AirMiniHistoryData &operator=(const AirMiniHistoryData &) = delete;
    AirMiniHistoryData(AirMiniHistoryData &&) = default;
    AirMiniHistoryData &operator=(AirMiniHistoryData &&) = default;

    void clear();

    bool begin(AirMiniHistoryTransferKind kind,
               uint32_t request_id,
               const AirMiniHistoryRange &range,
               const AirMiniHistorySelector *selectors,
               size_t selector_count);

    bool begin_logged_data(uint32_t request_id,
                           const AirMiniHistorySelector *selectors,
                           size_t selector_count);
    bool begin_logged_data(uint32_t request_id,
                           AirMiniHistorySelector selector);
    bool begin_settings_history(uint32_t request_id);

    AirMiniHistoryDecodeResult consume_response(const char *json,
                                                size_t json_len);
    AirMiniHistoryDecodeResult consume_notification(const char *json,
                                                    size_t json_len);

    const AirMiniHistorySeries &series(AirMiniHistorySelector selector) const;
    const AirMiniHistorySettings &settings() const { return settings_; }

    uint32_t rejected_mask() const;

    // Window for subsequent wire data. Retained rows are not pruned here.
    void set_window(int64_t from_ms, int64_t to_ms);

    bool logged_transfer_active() const { return logged_active_; }
    bool settings_transfer_active() const { return settings_active_; }
    uint32_t stream_id() const { return stream_id_; }
    uint32_t request_id() const { return request_id_; }
    const AirMiniHistoryRange &range() const { return range_; }

    // Periodic samples are merged by timestamp. Events and settings profiles
    // retain multiplicity, including equal-time records.
    bool merge_from(const AirMiniHistoryData &incoming);

    // Copy one day from a wider retained transfer. The copied window keeps
    // the nearest preceding MaskOn and settings profile as context.
    bool copy_window_from(const AirMiniHistoryData &incoming,
                          int64_t from_ms,
                          int64_t to_ms);

    bool serialize(LargeTextBuffer &out) const;
    bool deserialize(const char *json, size_t json_len);

    bool project_day(const AirMiniHistoryProjectionInput &input,
                     AirMiniHistoryStrProjection &out) const;

private:
    enum class ActiveTransfer : uint8_t {
        None,
        LoggedData,
        SettingsHistory,
    };

    AirMiniHistorySeries &mutable_series(AirMiniHistorySelector selector);
    const AirMiniHistorySeries &series_or_empty(
        AirMiniHistorySelector selector) const;
    bool selector_requested(AirMiniHistorySelector selector) const;
    void reset_logged_transfer();
    void reset_settings_transfer();

    AirMiniHistoryLimits limits_;
    AirMiniHistoryRange range_;
    AirMiniHistorySeries series_[static_cast<size_t>(
        AirMiniHistorySelector::Count)];
    AirMiniHistorySettings settings_;
    bool requested_[static_cast<size_t>(AirMiniHistorySelector::Count)] = {};
    bool logged_active_ = false;
    bool settings_active_ = false;
    ActiveTransfer active_transfer_ = ActiveTransfer::None;
    uint32_t request_id_ = 0;
    uint32_t stream_id_ = 0;
};

// Apply local EDF statistics first and use AirMini history only for fields and
// intervals not covered by those statistics. The accumulator is an existing
// STR seed; this function never replaces it with a reconstructed record.
bool airmini_history_prepare_str(const AirMiniHistoryData &history,
                                  const AirMiniHistoryProjectionInput &input,
                                  EdfDayStatisticsReader &local_statistics,
                                  AirMiniHistoryStrProjection &projection,
                                  const char *&error);

bool airmini_history_apply_str(const AirMiniHistoryData &history,
                               const AirMiniHistoryProjectionInput &input,
                               EdfDayStatisticsReader &local_statistics,
                               const AirMiniHistoryStrProjection &projection,
                               EdfStrSessionAccumulator &str,
                               const char *&error);

}  // namespace aircannect
