#include "report_night_epoch_state.h"

#include "memory_manager.h"
#include "report_diagnostics.h"

namespace aircannect {

ReportNightEpochState::~ReportNightEpochState() {
    Memory::free(nights_);
}

bool ReportNightEpochState::begin() {
    if (nights_) return true;

    nights_ = static_cast<NightEpoch *>(Memory::calloc_large(
        AC_REPORT_SUMMARY_RECORD_MAX,
        sizeof(NightEpoch),
        false));
    if (!nights_) {
        log_report_alloc_failed(
            "night_epochs",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(NightEpoch));
        return false;
    }

    return true;
}

void ReportNightEpochState::clear() {
    count_ = 0;
}

void ReportNightEpochState::note_chunk_committed(uint64_t night_start_ms) {
    data_epoch_++;
    if (!night_start_ms || !nights_) return;

    for (size_t i = 0; i < count_; ++i) {
        if (nights_[i].night_start_ms == night_start_ms) {
            nights_[i].epoch++;
            return;
        }
    }

    if (count_ >= AC_REPORT_SUMMARY_RECORD_MAX) return;

    nights_[count_].night_start_ms = night_start_ms;
    nights_[count_].epoch = 1;
    ++count_;
}

void ReportNightEpochState::remove_night(uint64_t night_start_ms) {
    if (!night_start_ms || !nights_) return;

    for (size_t i = 0; i < count_; ++i) {
        if (nights_[i].night_start_ms != night_start_ms) continue;

        nights_[i] = nights_[count_ - 1];
        --count_;
        return;
    }
}

uint32_t ReportNightEpochState::night_epoch(uint64_t night_start_ms) const {
    if (!night_start_ms || !nights_) return 0;

    for (size_t i = 0; i < count_; ++i) {
        if (nights_[i].night_start_ms == night_start_ms) {
            return nights_[i].epoch;
        }
    }

    return 0;
}

}  // namespace aircannect
