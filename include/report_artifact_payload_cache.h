#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "board_report.h"
#include "night_catalog.h"
#include "report_artifact_payload.h"

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

    bool can_hold(const ReportArtifactPayloadDescriptor &payload) const;
    bool contains(const ReportArtifactPayloadDescriptor &payload) const;
    bool contains_deflate(
        const ReportArtifactPayloadDescriptor &payload) const;
    bool can_hold(const ReportArtifactDescriptor &artifact) const {
        return can_hold(ReportArtifactPayloadDescriptor::whole(artifact));
    }
    bool contains(const ReportArtifactDescriptor &artifact) const {
        return contains(ReportArtifactPayloadDescriptor::whole(artifact));
    }
    bool describe(const ReportArtifactKey &artifact,
                  ReportArtifactDescriptor &out) const;
    bool describe_ready(const ReportArtifactKey &artifact,
                        ReportArtifactDescriptor &out) const;
    std::shared_ptr<const LargeByteBuffer> find(
        const ReportArtifactPayloadDescriptor &payload);
    std::shared_ptr<const LargeByteBuffer> find_if_present(
        const ReportArtifactPayloadDescriptor &payload);
    std::shared_ptr<const LargeByteBuffer> find(
        const ReportArtifactDescriptor &artifact);
    std::shared_ptr<const LargeByteBuffer> find_if_present(
        const ReportArtifactDescriptor &artifact);
    ReportArtifactPayloadSelection select(
        const ReportArtifactPayloadDescriptor &payload,
        bool prefer_deflate);
    ReportArtifactPayloadSelection select_if_present(
        const ReportArtifactPayloadDescriptor &payload,
        bool prefer_deflate);
    ReportArtifactPayloadSelection select(
        const ReportArtifactDescriptor &artifact,
        bool prefer_deflate);
    ReportArtifactPayloadSelection select_if_present(
        const ReportArtifactDescriptor &artifact,
        bool prefer_deflate);
    bool insert(const ReportArtifactPayloadDescriptor &payload,
                std::shared_ptr<const LargeByteBuffer> bytes);
    bool insert_deflate(
        const ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> bytes);
    bool insert(const ReportArtifactDescriptor &artifact,
                std::shared_ptr<const LargeByteBuffer> bytes) {
        return insert(ReportArtifactPayloadDescriptor::whole(artifact),
                      std::move(bytes));
    }
    bool insert_pair(const ReportArtifactDescriptor &result,
                     std::shared_ptr<const LargeByteBuffer> result_bytes,
                     const ReportArtifactDescriptor &overview,
                     std::shared_ptr<const LargeByteBuffer> overview_bytes);
    bool next_deflate_candidate(
        ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> &bytes) const;
    bool next_deflate_lookup_candidate(
        ReportArtifactPayloadDescriptor &payload) const;
    bool mark_deflate_pending(
        const ReportArtifactPayloadDescriptor &payload);
    bool mark_deflate_unavailable(
        const ReportArtifactPayloadDescriptor &payload);
    bool next_deflate_candidate(
        ReportArtifactDescriptor &artifact,
        std::shared_ptr<const LargeByteBuffer> &bytes) const;
    bool complete_deflate(
        const ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> bytes);
    bool complete_deflate(
        const ReportArtifactDescriptor &artifact,
        std::shared_ptr<const LargeByteBuffer> bytes) {
        return complete_deflate(
            ReportArtifactPayloadDescriptor::whole(artifact),
            std::move(bytes));
    }

    bool evict_lru();
    void reconcile(const NightCatalog &catalog);
    void clear();

    ReportArtifactPayloadCacheStatus status() const;

private:
    enum class DeflateState : uint8_t {
        Unchecked,
        Pending,
        Ready,
        Unavailable,
    };

    struct Entry {
        ReportArtifactPayloadDescriptor payload;
        std::shared_ptr<const LargeByteBuffer> bytes;
        std::shared_ptr<const LargeByteBuffer> deflated;
        DeflateState deflate_state = DeflateState::Unavailable;
        uint64_t last_used = 0;

        bool valid() const {
            return payload.valid() && (bytes != nullptr || deflated != nullptr);
        }
    };

    static bool same_descriptor(const ReportArtifactDescriptor &lhs,
                                const ReportArtifactDescriptor &rhs);
    static bool same_payload_identity(
        const ReportArtifactPayloadDescriptor &lhs,
        const ReportArtifactPayloadDescriptor &rhs);

    size_t find_exact(
        const ReportArtifactPayloadDescriptor &payload) const;
    size_t find_whole(
        const ReportArtifactPayloadDescriptor &payload) const;
    size_t find_key(const ReportArtifactKey &artifact) const;
    size_t find_free() const;
    size_t free_count() const;
    size_t find_lru(size_t excluded = SIZE_MAX) const;
    ReportArtifactPayloadSelection select_payload(
        const ReportArtifactPayloadDescriptor &payload,
        bool prefer_deflate,
        bool count_miss);
    ReportArtifactPayloadSelection select_at(size_t index,
                                              bool prefer_deflate,
                                              bool count_miss);
    void erase(size_t index, bool eviction);
    void prepare_entry(Entry &entry,
                       const ReportArtifactPayloadDescriptor &payload,
                       std::shared_ptr<const LargeByteBuffer> bytes);
    bool reserve(size_t wanted, size_t excluded = SIZE_MAX);
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
