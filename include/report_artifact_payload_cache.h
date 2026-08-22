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

enum class ReportArtifactPayloadEncoding : uint8_t {
    Identity,
    Deflate,
};

enum class ReportArtifactPayloadSelectionState : uint8_t {
    Missing,
    Pending,
    Ready,
};

struct ReportArtifactPayloadSelection {
    ReportArtifactPayloadSelectionState state =
        ReportArtifactPayloadSelectionState::Missing;
    ReportArtifactPayloadEncoding encoding =
        ReportArtifactPayloadEncoding::Identity;
    std::shared_ptr<const LargeByteBuffer> bytes;

    bool ready() const {
        return state == ReportArtifactPayloadSelectionState::Ready &&
               bytes != nullptr;
    }
};

// Stores immutable ready artifact bytes. ReportTask serializes mutations and
// protects cross-task reads with its own mutex.
class ReportArtifactPayloadCache {
public:
    explicit ReportArtifactPayloadCache(size_t byte_budget);

    bool can_hold(const ReportArtifactDescriptor &artifact) const;
    bool contains(const ReportArtifactDescriptor &artifact) const;
    bool describe_ready(const ReportArtifactKey &artifact,
                        ReportArtifactDescriptor &out) const;
    std::shared_ptr<const LargeByteBuffer> find(
        const ReportArtifactDescriptor &artifact);
    std::shared_ptr<const LargeByteBuffer> find_if_present(
        const ReportArtifactDescriptor &artifact);
    ReportArtifactPayloadSelection select(
        const ReportArtifactDescriptor &artifact,
        bool prefer_deflate);
    ReportArtifactPayloadSelection select_if_present(
        const ReportArtifactDescriptor &artifact,
        bool prefer_deflate);
    bool insert(const ReportArtifactDescriptor &artifact,
                std::shared_ptr<const LargeByteBuffer> bytes);
    bool insert_pair(const ReportArtifactDescriptor &result,
                     std::shared_ptr<const LargeByteBuffer> result_bytes,
                     const ReportArtifactDescriptor &overview,
                     std::shared_ptr<const LargeByteBuffer> overview_bytes);
    bool next_deflate_candidate(
        ReportArtifactDescriptor &artifact,
        std::shared_ptr<const LargeByteBuffer> &bytes) const;
    bool complete_deflate(
        const ReportArtifactDescriptor &artifact,
        std::shared_ptr<const LargeByteBuffer> bytes);

    bool evict_lru();
    void reconcile(const NightCatalog &catalog);
    void clear();

    ReportArtifactPayloadCacheStatus status() const;

private:
    enum class DeflateState : uint8_t {
        Pending,
        Ready,
        Unavailable,
    };

    struct Entry {
        ReportArtifactDescriptor artifact;
        std::shared_ptr<const LargeByteBuffer> bytes;
        std::shared_ptr<const LargeByteBuffer> deflated;
        DeflateState deflate_state = DeflateState::Unavailable;
        uint64_t last_used = 0;

        bool valid() const { return artifact.valid() && bytes != nullptr; }
    };

    static bool same_descriptor(const ReportArtifactDescriptor &lhs,
                                const ReportArtifactDescriptor &rhs);
    static bool same_key(const ReportArtifactDescriptor &lhs,
                         const ReportArtifactDescriptor &rhs);

    size_t find_exact(const ReportArtifactDescriptor &artifact) const;
    size_t find_key(const ReportArtifactKey &artifact) const;
    size_t find_free() const;
    size_t free_count() const;
    size_t find_lru(size_t excluded = SIZE_MAX) const;
    ReportArtifactPayloadSelection select_at(size_t index,
                                              bool prefer_deflate,
                                              bool count_miss);
    void erase(size_t index, bool eviction);
    void prepare_entry(Entry &entry,
                       const ReportArtifactDescriptor &artifact,
                       std::shared_ptr<const LargeByteBuffer> bytes);
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
