#pragma once

#include <stddef.h>
#include <stdint.h>

#include "operation_outcome.h"
#include "report_sources.h"
#include "report_spool_port.h"

namespace aircannect {

struct ReportParsedChunk;

enum class ReportSpoolBoundaryState : uint8_t {
    Unknown,
    Available,
    Empty,
};

struct ReportSpoolBoundary {
    ReportSpoolBoundaryState state = ReportSpoolBoundaryState::Unknown;
    int64_t oldest_ms = 0;

    bool known() const {
        return state != ReportSpoolBoundaryState::Unknown;
    }
};

class ReportSpoolAvailability {
public:
    void clear();
    void mark_available(ReportSourceId source, int64_t oldest_ms);
    void mark_empty(ReportSourceId source);

    ReportSpoolBoundary boundary(ReportSourceId source) const;
    bool excludes(ReportSourceId source, int64_t until_ms) const;

private:
    static constexpr size_t SourceCount =
        static_cast<size_t>(ReportSourceId::OximetryOneSecond) + 1;

    ReportSpoolBoundary boundaries_[SourceCount] = {};
};

enum class ReportSpoolAvailabilityProbeState : uint8_t {
    Idle,
    Fetching,
    Ready,
    Incomplete,
};

struct ReportSpoolAvailabilityProbeStatus {
    ReportSpoolAvailabilityProbeState state =
        ReportSpoolAvailabilityProbeState::Idle;
    ReportSourceId source = ReportSourceId::Summary;
    uint32_t generation = 0;
    uint8_t sources_completed = 0;
    uint8_t sources_known = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const {
        return state == ReportSpoolAvailabilityProbeState::Fetching;
    }
    bool terminal() const {
        return state == ReportSpoolAvailabilityProbeState::Ready ||
               state == ReportSpoolAvailabilityProbeState::Incomplete;
    }
};

class ReportSpoolAvailabilityProbe {
public:
    void begin(ReportSpoolPort &spool_port);

    OperationAdmission request(uint32_t generation);
    bool poll();
    void cancel();

    const ReportSpoolAvailabilityProbeStatus &status() const {
        return status_;
    }
    const ReportSpoolAvailability &availability() const {
        return availability_;
    }

private:
    static bool capture_oldest(void *context,
                               const ReportParsedChunk &chunk);

    bool submit_current();
    bool finish_current();
    bool parse_current(ReportSpoolResult &result, int64_t &oldest_ms,
                       bool &empty, char *error, size_t error_len);
    void advance(bool known, const char *error = nullptr);

    ReportSpoolPort *spool_port_ = nullptr;
    OperationTicket ticket_;
    ReportSpoolAvailability availability_;
    ReportSpoolAvailabilityProbeStatus status_;
    size_t source_index_ = 0;
    bool cancel_requested_ = false;
};

}  // namespace aircannect
