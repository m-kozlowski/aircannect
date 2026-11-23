#pragma once

#include "report_oximetry.h"
#include "storage_atomic_write_port.h"
#include "storage_range_write_port.h"

namespace aircannect {

// Source-scoped v9 supplement. Its publication record is written last.
// RAW timestamps for live observations are not snapped to the EDF grid.
struct ReportOximetryPlanes {
    static constexpr size_t PublicationBytes = 52;
    bool decode(const uint8_t *bytes, size_t length);
    uint32_t rows = 0, index_bytes = 0, histogram_bytes = 0;
    uint32_t plane_bytes[4]{}, compressed_bytes[4]{};
};

std::string report_oximetry_signal_path(const ReportOximetryRecording &recording,
                                        const char *plane);
struct ReportSignalTile;
bool report_oximetry_overview_tile(const ReportOximetryRecording &recording, ReportSignalTile &tile);
inline constexpr const char *ReportOximetryChannels[] = {"spo2", "pulse", "movement", "pi"};

class ReportOximetrySignalBuilder {
public:
    enum class Mode { Build, Manifest, VerifyOriginal };
    ~ReportOximetrySignalBuilder();
    void begin(StorageReadPort &reads, StorageRangeWritePort &ranges,
               StorageAtomicWritePort &writes);
    void reset();
    void pause();
    bool step(ReportOximetryRecording &recording, Mode mode = Mode::Build);
    bool finished() const;
    bool retryable() const;
    bool source_changed() const;
    bool verified() const;

private:
    struct Work;
    bool complete_write(Work &work);
    Work *work_ = nullptr;
    StorageReadPort *reads_ = nullptr;
    StorageRangeWritePort *ranges_ = nullptr;
    StorageAtomicWritePort *writes_ = nullptr;
};

}  // namespace aircannect
