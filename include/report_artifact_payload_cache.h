#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "board_report.h"
#include "night_catalog.h"
#include "report_artifacts.h"

namespace aircannect {

struct ReportArtifactPayloadCacheStatus {
    size_t entries = 0;
    size_t bytes = 0;
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t evictions = 0;
};

// Stores immutable ready artifact bytes. ReportTask serializes mutations and
// protects cross-task reads with its own mutex.
class ReportArtifactPayloadCache {
public:
    explicit ReportArtifactPayloadCache(size_t byte_budget);

    bool can_hold(const ReportArtifactDescriptor &artifact) const;
    bool contains(const ReportArtifactDescriptor &artifact) const;
    std::shared_ptr<const LargeByteBuffer> find(
        const ReportArtifactDescriptor &artifact);
    bool insert(const ReportArtifactDescriptor &artifact,
                std::shared_ptr<const LargeByteBuffer> bytes);

    bool evict_lru();
    void reconcile(const NightCatalog &catalog);
    void clear();

    ReportArtifactPayloadCacheStatus status() const;

private:
    struct Entry {
        ReportArtifactDescriptor artifact;
        std::shared_ptr<const LargeByteBuffer> bytes;
        uint64_t last_used = 0;

        bool valid() const { return artifact.valid() && bytes != nullptr; }
    };

    static bool same_descriptor(const ReportArtifactDescriptor &lhs,
                                const ReportArtifactDescriptor &rhs);
    static bool same_key(const ReportArtifactDescriptor &lhs,
                         const ReportArtifactDescriptor &rhs);

    size_t find_exact(const ReportArtifactDescriptor &artifact) const;
    size_t find_free() const;
    size_t find_lru() const;
    void erase(size_t index, bool eviction);
    uint64_t next_use();

    Entry entries_[AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY] = {};
    size_t byte_budget_ = 0;
    size_t bytes_ = 0;
    uint64_t use_clock_ = 0;
    uint32_t hits_ = 0;
    uint32_t misses_ = 0;
    uint32_t evictions_ = 0;
};

}  // namespace aircannect
