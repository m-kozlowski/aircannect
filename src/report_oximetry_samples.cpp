#include "report_oximetry_samples.h"

#include <cmath>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include "utc_time.h"

namespace aircannect {

void ReportOximetrySampleReader::begin(const ReportOximetryRecording &recording) {
    *this = {};
    recording_ = &recording;
}

bool ReportOximetrySampleReader::csv_row(ReportOximetrySample &sample) {
    char *field[8];
    field[0] = row_;
    for (size_t i = 1; i < 8; ++i) {
        char *comma = strchr(field[i-1], ',');
        if (!comma) return false;
        *comma = 0;
        field[i] = comma + 1;
    }
    if (!parse_utc_iso8601_ms(field[6], sample.time_ms) ||
        sample.time_ms < recording_->start_ms || sample.time_ms < last_ms_ ||
        sample.time_ms >= recording_->csv_end_ms) return false;

    const unsigned indices[] = {1, 2, 3, 7};
    for (size_t i = 0; i < ReportOximetrySample::Channels; ++i) {
        const char *value = field[indices[i]];
        if (!*value) continue;
        const char *end = i == 3 ? value + strlen(value) : field[indices[i] + 1] - 1;
        unsigned integer;
        const auto parsed = std::from_chars(value, end, integer);
        float v;
        if (parsed.ec == std::errc{} && parsed.ptr == end && integer <= (1u << 24)) {
            v = static_cast<float>(integer);
        } else {
            char *tail;
            v = strtof(value, &tail);
            if (*tail) return false;
        }
        if (!std::isfinite(v) || v < 0) return false;
        if (i == ReportOximetrySample::Oxygen && (v < 1 || v > 100)) continue;
        if (i == ReportOximetrySample::Pulse && (v < 1 || v == 255)) continue;
        sample.values[i] = v;
    }
    return true;
}

bool ReportOximetrySampleReader::consume(const uint8_t *bytes, size_t size,
                                         void *context, Sink sink) {
    while (size) {
        ReportOximetrySample sample;
        if (recording_->csv_bytes) {
            const auto *nl = static_cast<const uint8_t *>(memchr(bytes, '\n', size));
            const size_t n = nl ? size_t(nl - bytes) + 1 : size;
            if (used_ + n >= sizeof(row_)) return false;
            memcpy(row_ + used_, bytes, n);
            bytes += n;
            size -= n;
            used_ += n;
            if (!nl) continue;

            const std::string_view line(row_, used_);
            used_ = 0;
            if (!columns_) {
                if (line == OximetryCsv::Columns) columns_ = true;
                else if (!comment_ && line.substr(0, 9) == "# Source=") comment_ = true;
                else return false;
                continue;
            }
            if (line.size() < 2 || line.substr(line.size()-2) != "\r\n") return false;
            row_[line.size()-2] = 0;
            if (!csv_row(sample)) return false;
        } else {
            const auto &bin = recording_->bin;
            if (size < bin.stride() || samples_ >= recording_->samples) return false;
            const auto value = bin.decode(bytes);
            sample.duration_ms = recording_->interval_s * 1000;
            sample.time_ms = recording_->start_ms + int64_t(samples_) * sample.duration_ms;

            if (!value.invalid_vitals) {
                if (value.oxygen && value.oxygen <= 100) sample.values[0] = value.oxygen;
                if (value.pulse && value.pulse < 255) sample.values[1] = value.pulse;
            }
            sample.values[2] = value.motion;
            bytes += bin.stride();
            size -= bin.stride();
        }

        if (!sink(context, sample)) return false;
        last_ms_ = sample.time_ms;
        ++samples_;
    }
    return true;
}

bool ReportOximetrySampleReader::complete() const {
    return recording_->csv_bytes ? columns_ && !used_ && samples_ && last_ms_ + 1 == recording_->csv_end_ms
                                 : samples_ == recording_->samples;
}

}  // namespace aircannect
