#pragma once

#include <stdint.h>

#include "report_artifacts.h"

namespace aircannect {

enum class ReportPayloadKind : uint8_t {
    Whole,
    PlotIndex,
    PlotEvents,
    PlotSeries,
};

struct ReportArtifactPayloadDescriptor {
    ReportArtifactDescriptor artifact;
    ReportPayloadKind kind = ReportPayloadKind::Whole;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t crc32 = 0;

    static ReportArtifactPayloadDescriptor whole(
        const ReportArtifactDescriptor &artifact) {
        ReportArtifactPayloadDescriptor out;
        out.artifact = artifact;
        out.size = static_cast<uint32_t>(artifact.size);
        out.crc32 = artifact.crc32;
        return out;
    }

    bool valid() const {
        return artifact.valid() && size > 0 && offset <= artifact.size &&
               size <= artifact.size - offset;
    }

    bool is_whole() const {
        return kind == ReportPayloadKind::Whole && offset == 0 &&
               size == artifact.size && crc32 == artifact.crc32;
    }

    bool operator==(const ReportArtifactPayloadDescriptor &other) const {
        return artifact.key == other.artifact.key &&
               artifact.size == other.artifact.size &&
               artifact.crc32 == other.artifact.crc32 &&
               artifact.prefix_crc32 == other.artifact.prefix_crc32 &&
               kind == other.kind && offset == other.offset &&
               size == other.size && crc32 == other.crc32;
    }
};

}  // namespace aircannect
