#pragma once

#include <memory>
#include <string>

#include "large_byte_buffer.h"
#include "storage_atomic_write_port.h"
#include "storage_range_write_port.h"
#include "storage_read_port.h"
#include "summary_fetch_reason.h"

namespace aircannect {

// Facts observed at each end of a transfer, not inferred historical timezone.
struct SummaryClockObservation {
    int64_t utc_ms = 0;
    uint32_t uptime_ms = 0;
    bool utc_valid = false;
    bool ntp_synced = false;
    std::string clock_source;
    std::string timezone;
    int32_t timezone_offset_min = 0;
    bool timezone_offset_valid = false;
    std::string device_time;
    uint32_t device_sample_age_ms = 0;
    bool device_offset_valid = false;
    int64_t device_minus_utc_ms = 0;
    bool device_timezone_valid = false;
    int32_t device_timezone_min = 0;
};

struct SummaryObservation {
    std::string device_serial;
    std::string firmware;
    std::string device_firmware;
    uint32_t boot_id = 0;
    SummaryFetchReason reason = SummaryFetchReason::Unspecified;
    uint32_t session_id = 0;
    std::string session_start;
    std::string session_end;
    int64_t from_ms = 0;
    SummaryClockObservation started;
    SummaryClockObservation finished;
    std::shared_ptr<const LargeByteBuffer> payload;
    bool empty_payload = false;
};

struct SummaryArchiveStatus {
    bool active = false;
    uint64_t sequence = 0;
    uint64_t committed_bytes = 0;
    size_t records = 0;
    size_t changed = 0;
    size_t removed = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// Source archive, independent of report generations and their cleanup.
// One worker owns the index; all filesystem access goes through storage ports.
class SummaryArchive {
public:
    SummaryArchive();
    ~SummaryArchive();
    SummaryArchive(const SummaryArchive &) = delete;
    SummaryArchive &operator=(const SummaryArchive &) = delete;

    void begin(StorageReadPort &read, StorageRangeWritePort &range,
               StorageAtomicWritePort &atomic);
    bool start(SummaryObservation observation);
    bool poll();
    void forget_media();
    const SummaryArchiveStatus &status() const;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

}  // namespace aircannect
