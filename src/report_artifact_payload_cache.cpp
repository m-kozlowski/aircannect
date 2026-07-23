#include "report_artifact_payload_cache.h"

namespace aircannect {

ReportArtifactPayloadCache::ReportArtifactPayloadCache(size_t byte_budget) :
    byte_budget_(byte_budget) {}

bool ReportArtifactPayloadCache::same_descriptor(
    const ReportArtifactDescriptor &lhs,
    const ReportArtifactDescriptor &rhs) {
    return lhs.key == rhs.key && lhs.size == rhs.size &&
           lhs.crc32 == rhs.crc32;
}

bool ReportArtifactPayloadCache::same_key(
    const ReportArtifactDescriptor &lhs,
    const ReportArtifactDescriptor &rhs) {
    return lhs.key == rhs.key;
}

size_t ReportArtifactPayloadCache::find_exact(
    const ReportArtifactDescriptor &artifact) const {
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (entries_[i].valid() &&
            same_descriptor(entries_[i].artifact, artifact)) {
            return i;
        }
    }
    return SIZE_MAX;
}

size_t ReportArtifactPayloadCache::find_free() const {
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (!entries_[i].valid()) return i;
    }
    return SIZE_MAX;
}

size_t ReportArtifactPayloadCache::find_lru() const {
    size_t selected = SIZE_MAX;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (!entries_[i].valid() || entries_[i].last_used >= oldest) {
            continue;
        }

        selected = i;
        oldest = entries_[i].last_used;
    }
    return selected;
}

void ReportArtifactPayloadCache::erase(size_t index, bool eviction) {
    if (index >= AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY ||
        !entries_[index].valid()) {
        return;
    }

    bytes_ -= entries_[index].bytes->size();
    entries_[index] = {};
    if (eviction) evictions_++;
}

uint64_t ReportArtifactPayloadCache::next_use() {
    use_clock_++;
    if (use_clock_ != 0) return use_clock_;

    use_clock_ = 1;
    for (Entry &entry : entries_) {
        if (entry.valid()) entry.last_used = use_clock_;
    }
    return use_clock_;
}

bool ReportArtifactPayloadCache::can_hold(
    const ReportArtifactDescriptor &artifact) const {
    return artifact.valid() && artifact.size <= SIZE_MAX &&
           artifact.size <= byte_budget_;
}

bool ReportArtifactPayloadCache::contains(
    const ReportArtifactDescriptor &artifact) const {
    return find_exact(artifact) != SIZE_MAX;
}

std::shared_ptr<const LargeByteBuffer> ReportArtifactPayloadCache::find(
    const ReportArtifactDescriptor &artifact) {
    const size_t index = find_exact(artifact);
    if (index == SIZE_MAX) {
        misses_++;
        return {};
    }

    entries_[index].last_used = next_use();
    hits_++;
    return entries_[index].bytes;
}

bool ReportArtifactPayloadCache::insert(
    const ReportArtifactDescriptor &artifact,
    std::shared_ptr<const LargeByteBuffer> bytes) {
    if (!can_hold(artifact) || !bytes || bytes->size() != artifact.size) {
        return false;
    }

    const size_t exact = find_exact(artifact);
    if (exact != SIZE_MAX) {
        entries_[exact].last_used = next_use();
        return true;
    }

    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (entries_[i].valid() &&
            same_key(entries_[i].artifact, artifact)) {
            erase(i, false);
        }
    }

    while (bytes_ + bytes->size() > byte_budget_ ||
           find_free() == SIZE_MAX) {
        if (!evict_lru()) return false;
    }

    const size_t index = find_free();
    if (index == SIZE_MAX) return false;

    entries_[index].artifact = artifact;
    entries_[index].bytes = std::move(bytes);
    entries_[index].last_used = next_use();
    bytes_ += entries_[index].bytes->size();
    return true;
}

bool ReportArtifactPayloadCache::evict_lru() {
    const size_t index = find_lru();
    if (index == SIZE_MAX) return false;

    erase(index, true);
    return true;
}

void ReportArtifactPayloadCache::reconcile(const NightCatalog &catalog) {
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (!entries_[i].valid()) continue;

        const NightCatalogRecord *night =
            catalog.find(entries_[i].artifact.key.sleep_day);
        if (!night || night->source_revision !=
                          entries_[i].artifact.key.source_revision) {
            erase(i, false);
        }
    }
}

void ReportArtifactPayloadCache::clear() {
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        erase(i, false);
    }
}

ReportArtifactPayloadCacheStatus ReportArtifactPayloadCache::status() const {
    ReportArtifactPayloadCacheStatus out;
    out.bytes = bytes_;
    out.hits = hits_;
    out.misses = misses_;
    out.evictions = evictions_;
    for (const Entry &entry : entries_) {
        if (entry.valid()) out.entries++;
    }
    return out;
}

}  // namespace aircannect
