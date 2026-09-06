#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "night_catalog.h"
#include "report_night_summary.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

class ReportSignalStoreService;

static constexpr const char *REPORT_SIGNAL_STORE_ROOT =
    "/aircannect/report/v9/nights";
static constexpr int64_t REPORT_SIGNAL_STORE_BLOCK_MS =
    15LL * 60LL * 1000LL;
static constexpr size_t REPORT_SIGNAL_STORE_MAX_BLOCKS = 128;
static constexpr size_t REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES =
    REPORT_SIGNAL_STORE_MAX_BLOCKS / 8;
static constexpr int16_t REPORT_SIGNAL_STORE_MISSING_S16 = INT16_MIN;

enum class ReportSignalStoreEncoding : uint8_t {
    Signed16 = 1,
};

enum class ReportSignalStoreUnit : uint8_t {
    None = 0,
    LitresPerMinute,
    CentimetresWater,
    Seconds,
    BreathsPerMinute,
    Ratio,
    Litres,
    Percent,
    BeatsPerMinute,
};

enum class ReportSignalStoreLevel : uint8_t {
    Raw = 0,
    OneSecond,
    TenSeconds,
};

enum ReportSignalStoreLodFlag : uint8_t {
    REPORT_SIGNAL_STORE_LOD_1S = 1u << 0,
    REPORT_SIGNAL_STORE_LOD_10S = 1u << 1,
};

struct ReportSignalStoreTrack {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    ReportSignalId signal = ReportSignalId::Invalid;
    ReportSignalStoreEncoding encoding =
        ReportSignalStoreEncoding::Signed16;
    ReportSignalStoreUnit unit = ReportSignalStoreUnit::None;
    uint8_t lod_mask = 0;
    uint16_t track_index = 0;
    uint16_t block_slot_count = 0;
    uint16_t present_block_count = 0;
    uint32_t generation = 0;
    uint32_t sample_interval_ms = 0;
    float value_scale = 0.001f;
    float value_offset = 0.0f;
    int16_t missing_value = REPORT_SIGNAL_STORE_MISSING_S16;
    uint32_t grid_phase_ms = 0;
    int64_t first_block_start_ms = 0;
    int64_t first_valid_sample_ms = 0;
    int64_t last_valid_sample_ms = 0;
    uint64_t valid_sample_count = 0;
    uint64_t expected_sample_count = 0;
    uint8_t present_blocks[REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES] = {};
};

struct ReportSignalStoreFilePayload {
    ReportSignalStoreTrack track;

    bool valid() const;
};

class ReportSignalStoreBundle {
public:
    ReportSignalStoreBundle() = default;
    ~ReportSignalStoreBundle();

    ReportSignalStoreBundle(const ReportSignalStoreBundle &) = delete;
    ReportSignalStoreBundle &operator=(
        const ReportSignalStoreBundle &) = delete;

    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    std::shared_ptr<const LargeByteBuffer> metadata;
    std::shared_ptr<const LargeByteBuffer> events;
    std::shared_ptr<const LargeByteBuffer> checkpoint;
    uint8_t checkpoint_slot = 0;

    size_t signal_count() const { return signal_count_; }
    const ReportSignalStoreFilePayload *signal(size_t index) const;
    bool valid() const;

private:
    bool allocate_signals(size_t count);
    void release_events();

    ReportSignalStoreFilePayload *signals_ = nullptr;
    size_t signal_count_ = 0;

    friend class ReportSignalStoreBuilder;
    friend class ReportSignalStoreService;
};

struct ReportSignalStorePlaneRange {
    size_t offset = 0;
    size_t length = 0;
    uint32_t interval_ms = 0;
    uint32_t cell_count = 0;
    bool envelope = false;
};

struct ReportSignalStoreFileView {
    ReportSignalStoreTrack track;
    ReportSignalStoreLevel level = ReportSignalStoreLevel::Raw;
    const uint8_t *bytes = nullptr;
    size_t length = 0;
    uint32_t plane_block_bytes = 0;
    uint32_t plane_interval_ms = 0;
    uint32_t plane_cell_count = 0;
    uint32_t samples_per_block = 0;
};

class ReportSignalStoreFileCodec {
public:
    static constexpr uint16_t Version = 2;
    static constexpr size_t HeaderBytes = 192;
    static constexpr size_t MaxBlockBytes = 45000;

    // One file per level; slot is the bitmap slot, not the packed ordinal.
    static std::shared_ptr<const LargeByteBuffer> encode_header(
        const ReportSignalStoreTrack &track,
        ReportSignalStoreLevel level);
    static std::shared_ptr<const LargeByteBuffer> encode_block(
        const ReportSignalStoreTrack &track,
        ReportSignalStoreLevel level,
        size_t slot,
        const int16_t *raw);

    // Optional OneSecond output for the same track/slots, with or without its
    // combined header. Used only when encoding TenSeconds.
    static std::shared_ptr<const LargeByteBuffer> encode_blocks(
        const ReportSignalStoreTrack &track,
        ReportSignalStoreLevel level,
        size_t first_slot,
        const int16_t *const *raw_blocks,
        size_t block_count,
        bool include_header = false,
        const LargeByteBuffer *one_second = nullptr);

    static bool inspect(const uint8_t *bytes,
                        size_t length,
                        ReportSignalStoreFileView &view);

    static bool file_size(const ReportSignalStoreTrack &track,
                          ReportSignalStoreLevel level,
                          size_t &size);
    static bool plane_range(const ReportSignalStoreFileView &view,
                            int64_t block_start_ms,
                            ReportSignalStoreLevel level,
                            ReportSignalStorePlaneRange &range);
    static bool plane_range(const ReportSignalStoreTrack &track,
                            int64_t first_block_start_ms,
                            size_t block_count,
                            ReportSignalStoreLevel level,
                            ReportSignalStorePlaneRange &range);

    static bool sample(const ReportSignalStoreFileView &view,
                       int64_t timestamp_ms,
                       bool &present,
                       int32_t &value_milli);
    static bool sample_raw(const ReportSignalStoreFileView &view,
                           int64_t timestamp_ms,
                           bool &present,
                           int16_t &raw);
};

enum ReportSignalStoreNightFlag : uint32_t {
    REPORT_SIGNAL_STORE_NIGHT_SUMMARY_AVAILABLE = 1u << 0,
    REPORT_SIGNAL_STORE_NIGHT_OPEN_SESSION = 1u << 1,
};

struct ReportSignalStoreNight {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    uint64_t closed_therapy_duration_ms = 0;
    uint32_t generation = 0;
    uint32_t flags = 0;
    int32_t timezone_offset_minutes = 0;
    uint8_t available_event_mask = 0;
    uint8_t source_flags = 0;
    uint8_t checkpoint_slot = 0;
    uint32_t event_count = 0;
    uint32_t requested_signal_mask = 0;
    uint32_t missing_required_signal_mask = 0;
    uint32_t missing_optional_signal_mask = 0;
    uint8_t requested_event_mask = 0;
    uint8_t missing_event_mask = 0;
    ReportNightMetrics metrics;
    ReportEventCounts events;
    const NightCatalogTimeRange *sessions = nullptr;
    size_t session_count = 0;
    const ReportSignalStoreTrack *tracks = nullptr;
    size_t track_count = 0;
};

struct ReportSignalStoreNightView {
    ReportSignalStoreNight night;
    const uint8_t *session_records = nullptr;
    const uint8_t *track_records = nullptr;

    bool session(size_t index, NightCatalogTimeRange &range) const;
    bool track(size_t index, ReportSignalStoreTrack &track) const;
};

class ReportSignalStoreNightCodec {
public:
    static constexpr size_t MaxBytes = 64 * 1024;
    static constexpr uint16_t Version = 2;
    static constexpr size_t HeaderBytes = 224;
    static constexpr size_t SessionBytes = 16;
    static constexpr size_t TrackBytes = 88;

    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportSignalStoreNight &night);
    static bool decode(const uint8_t *bytes,
                       size_t length,
                       ReportSignalStoreNightView &view);
};

struct ReportSignalStoreEventFileData {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    int64_t first_block_start_ms = 0;
    uint16_t block_slot_count = 0;
    const ReportEventRecord *events = nullptr;
    size_t event_count = 0;
};

struct ReportSignalStoreEventFileView {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    int64_t first_block_start_ms = 0;
    uint16_t block_slot_count = 0;
    uint32_t event_count = 0;
    const uint8_t *bytes = nullptr;
    size_t length = 0;
    const uint8_t *block_directory = nullptr;
    const uint8_t *event_records = nullptr;
    uint8_t present_blocks[REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES] = {};
};

class ReportSignalStoreEventCodec {
public:
    static constexpr uint16_t Version = 1;
    static constexpr size_t HeaderBytes = 96;
    static constexpr size_t BlockDirectoryBytes = 8;
    static constexpr size_t EventBytes = 16;

    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportSignalStoreEventFileData &data);
    static bool inspect(const uint8_t *bytes,
                        size_t length,
                        ReportSignalStoreEventFileView &view);
    static bool file_size(uint16_t block_slot_count,
                          uint32_t event_count,
                          size_t &size);
    static bool block(const ReportSignalStoreEventFileView &view,
                      int64_t block_start_ms,
                      uint32_t &first_event,
                      uint32_t &event_count);
    static bool event(const ReportSignalStoreEventFileView &view,
                      size_t index,
                      ReportEventRecord &event);
};

bool report_signal_store_night_path(SleepDayId sleep_day,
                                    char *out,
                                    size_t out_size);
bool report_signal_store_signal_path(const ReportSignalStoreTrack &track,
                                     ReportSignalStoreLevel level,
                                     char *out,
                                     size_t out_size);
bool report_signal_store_events_path(SleepDayId sleep_day,
                                     uint32_t generation,
                                     char *out,
                                     size_t out_size);

bool report_signal_store_track_valid(const ReportSignalStoreTrack &track);
ReportSignalStoreUnit report_signal_store_unit(ReportSignalId signal);

}  // namespace aircannect
