#include "report_artifact_payload_cache.h"

#include "board_report.h"

namespace aircannect {

ReportArtifactPayloadCache::ReportArtifactPayloadCache(size_t byte_budget) :
    byte_budget_(byte_budget) {}

bool ReportArtifactPayloadCache::same_descriptor(
    const ReportArtifactDescriptor &lhs,
    const ReportArtifactDescriptor &rhs) {
    return lhs.key == rhs.key && lhs.size == rhs.size &&
           lhs.crc32 == rhs.crc32 &&
           lhs.prefix_crc32 == rhs.prefix_crc32;
}

bool ReportArtifactPayloadCache::same_payload_identity(
    const ReportArtifactPayloadDescriptor &lhs,
    const ReportArtifactPayloadDescriptor &rhs) {
    return lhs.artifact.key == rhs.artifact.key &&
           lhs.kind == rhs.kind && lhs.offset == rhs.offset;
}

size_t ReportArtifactPayloadCache::find_exact(
    const ReportArtifactPayloadDescriptor &payload) const {
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (entries_[i].valid() && entries_[i].payload == payload) return i;
    }
    return SIZE_MAX;
}

size_t ReportArtifactPayloadCache::find_whole(
    const ReportArtifactPayloadDescriptor &payload) const {
    if (payload.is_whole()) return SIZE_MAX;
    const size_t index = find_exact(
        ReportArtifactPayloadDescriptor::whole(payload.artifact));
    return index != SIZE_MAX && entries_[index].bytes ? index : SIZE_MAX;
}

size_t ReportArtifactPayloadCache::find_key(
    const ReportArtifactKey &artifact) const {
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (entries_[i].valid() && entries_[i].payload.is_whole() &&
            entries_[i].payload.artifact.key == artifact) {
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

size_t ReportArtifactPayloadCache::free_count() const {
    size_t count = 0;
    for (const Entry &entry : entries_) {
        if (!entry.valid()) ++count;
    }
    return count;
}

size_t ReportArtifactPayloadCache::find_lru(size_t excluded) const {
    size_t selected = SIZE_MAX;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (i == excluded || !entries_[i].valid() ||
            entries_[i].last_used >= oldest) {
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

    if (entries_[index].bytes) {
        bytes_ -= entries_[index].bytes->size();
    }
    if (entries_[index].deflated) {
        bytes_ -= entries_[index].deflated->size();
    }
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
    const ReportArtifactPayloadDescriptor &payload) const {
    return payload.valid() && payload.size <= byte_budget_;
}

bool ReportArtifactPayloadCache::contains(
    const ReportArtifactPayloadDescriptor &payload) const {
    const size_t exact = find_exact(payload);
    return (exact != SIZE_MAX && entries_[exact].bytes) ||
           find_whole(payload) != SIZE_MAX;
}

bool ReportArtifactPayloadCache::contains_deflate(
    const ReportArtifactPayloadDescriptor &payload) const {
    const size_t exact = find_exact(payload);
    return exact != SIZE_MAX &&
           entries_[exact].deflate_state == DeflateState::Ready &&
           entries_[exact].deflated != nullptr;
}

bool ReportArtifactPayloadCache::describe(
    const ReportArtifactKey &artifact,
    ReportArtifactDescriptor &out) const {
    out = {};
    if (!artifact.valid()) return false;

    const size_t requested = find_key(artifact);
    if (requested == SIZE_MAX) return false;

    out = entries_[requested].payload.artifact;
    return true;
}

bool ReportArtifactPayloadCache::describe_ready(
    const ReportArtifactKey &artifact,
    ReportArtifactDescriptor &out) const {
    if (!describe(artifact, out)) return false;

    const ReportArtifactKey result = ReportArtifactKey::result(
        artifact.sleep_day, artifact.source_revision);
    const ReportArtifactKey overview = ReportArtifactKey::overview(
        artifact.sleep_day, artifact.source_revision);
    return find_key(result) != SIZE_MAX && find_key(overview) != SIZE_MAX;
}

std::shared_ptr<const LargeByteBuffer> ReportArtifactPayloadCache::find(
    const ReportArtifactPayloadDescriptor &payload) {
    size_t index = find_exact(payload);
    if (index != SIZE_MAX && entries_[index].bytes) {
        entries_[index].last_used = next_use();
        hits_++;
        return entries_[index].bytes;
    }

    index = find_whole(payload);
    if (index != SIZE_MAX) {
        entries_[index].last_used = next_use();
        hits_++;
        return LargeByteBuffer::slice(
            entries_[index].bytes, payload.offset, payload.size);
    }

    misses_++;
    return {};
}

std::shared_ptr<const LargeByteBuffer>
ReportArtifactPayloadCache::find_if_present(
    const ReportArtifactPayloadDescriptor &payload) {
    size_t index = find_exact(payload);
    if (index != SIZE_MAX && entries_[index].bytes) {
        entries_[index].last_used = next_use();
        hits_++;
        return entries_[index].bytes;
    }

    index = find_whole(payload);
    if (index == SIZE_MAX) return {};

    entries_[index].last_used = next_use();
    hits_++;
    return LargeByteBuffer::slice(
        entries_[index].bytes, payload.offset, payload.size);
}

std::shared_ptr<const LargeByteBuffer> ReportArtifactPayloadCache::find(
    const ReportArtifactDescriptor &artifact) {
    return find(ReportArtifactPayloadDescriptor::whole(artifact));
}

std::shared_ptr<const LargeByteBuffer>
ReportArtifactPayloadCache::find_if_present(
    const ReportArtifactDescriptor &artifact) {
    return find_if_present(ReportArtifactPayloadDescriptor::whole(artifact));
}

ReportArtifactPayloadSelection ReportArtifactPayloadCache::select_at(
    size_t index,
    bool prefer_deflate,
    bool count_miss) {
    ReportArtifactPayloadSelection out;
    if (index == SIZE_MAX) {
        if (count_miss) misses_++;
        return out;
    }

    Entry &entry = entries_[index];
    if (prefer_deflate && entry.deflate_state == DeflateState::Ready &&
        entry.deflated) {
        out.state = ReportArtifactPayloadSelectionState::Ready;
        out.encoding = ReportArtifactPayloadEncoding::Deflate;
        out.bytes = entry.deflated;
    } else if (entry.bytes) {
        out.state = ReportArtifactPayloadSelectionState::Ready;
        out.encoding = ReportArtifactPayloadEncoding::Identity;
        out.bytes = entry.bytes;
    } else {
        if (count_miss) misses_++;
        return out;
    }
    entry.last_used = next_use();
    hits_++;
    return out;
}

ReportArtifactPayloadSelection ReportArtifactPayloadCache::select_payload(
    const ReportArtifactPayloadDescriptor &payload,
    bool prefer_deflate,
    bool count_miss) {
    const size_t exact = find_exact(payload);
    if (exact != SIZE_MAX) {
        ReportArtifactPayloadSelection selected =
            select_at(exact, prefer_deflate, false);
        if (selected.ready()) return selected;
    }

    const size_t whole = find_whole(payload);
    if (whole == SIZE_MAX) {
        if (count_miss) misses_++;
        return {};
    }

    ReportArtifactPayloadSelection out;
    out.state = ReportArtifactPayloadSelectionState::Ready;
    out.encoding = ReportArtifactPayloadEncoding::Identity;
    out.bytes = LargeByteBuffer::slice(
        entries_[whole].bytes, payload.offset, payload.size);
    if (!out.bytes) return {};

    entries_[whole].last_used = next_use();
    hits_++;
    return out;
}

ReportArtifactPayloadSelection ReportArtifactPayloadCache::select(
    const ReportArtifactPayloadDescriptor &payload,
    bool prefer_deflate) {
    return select_payload(payload, prefer_deflate, true);
}

ReportArtifactPayloadSelection
ReportArtifactPayloadCache::select_if_present(
    const ReportArtifactPayloadDescriptor &payload,
    bool prefer_deflate) {
    return select_payload(payload, prefer_deflate, false);
}

ReportArtifactPayloadSelection ReportArtifactPayloadCache::select(
    const ReportArtifactDescriptor &artifact,
    bool prefer_deflate) {
    return select(
        ReportArtifactPayloadDescriptor::whole(artifact), prefer_deflate);
}

ReportArtifactPayloadSelection
ReportArtifactPayloadCache::select_if_present(
    const ReportArtifactDescriptor &artifact,
    bool prefer_deflate) {
    return select_if_present(
        ReportArtifactPayloadDescriptor::whole(artifact), prefer_deflate);
}

void ReportArtifactPayloadCache::prepare_entry(
    Entry &entry,
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> bytes) {
    entry.payload = payload;
    entry.bytes = std::move(bytes);
    entry.deflated.reset();
    entry.deflate_state = payload.size >= AC_REPORT_HTTP_DEFLATE_MIN_BYTES
        ? DeflateState::Unchecked
        : DeflateState::Unavailable;
    entry.last_used = next_use();
}

bool ReportArtifactPayloadCache::insert(
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> bytes) {
    if (!can_hold(payload) || !bytes || bytes->size() != payload.size) {
        return false;
    }

    const size_t exact = find_exact(payload);
    if (exact != SIZE_MAX) {
        if (!entries_[exact].bytes) {
            if (!reserve(bytes->size(), exact)) return false;
            entries_[exact].bytes = std::move(bytes);
            bytes_ += entries_[exact].bytes->size();
        }
        entries_[exact].last_used = next_use();
        return true;
    }

    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (entries_[i].valid() &&
            same_payload_identity(entries_[i].payload, payload)) {
            erase(i, false);
        }
    }

    if (!reserve(bytes->size())) return false;

    const size_t index = find_free();
    if (index == SIZE_MAX) return false;

    prepare_entry(entries_[index], payload, std::move(bytes));
    bytes_ += entries_[index].bytes->size();
    return true;
}

bool ReportArtifactPayloadCache::insert_deflate(
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> bytes) {
    if (!payload.valid() || !bytes || bytes->size() == 0 ||
        bytes->size() >= payload.size || bytes->size() > byte_budget_) {
        return false;
    }

    size_t exact = find_exact(payload);
    if (exact == SIZE_MAX) {
        for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
            if (entries_[i].valid() &&
                same_payload_identity(entries_[i].payload, payload)) {
                erase(i, false);
            }
        }

        if (!reserve(bytes->size())) return false;
        exact = find_free();
        if (exact == SIZE_MAX) return false;
        entries_[exact].payload = payload;
    } else if (entries_[exact].deflated) {
        entries_[exact].last_used = next_use();
        return true;
    } else if (!reserve(bytes->size(), exact)) {
        return false;
    }

    Entry &entry = entries_[exact];
    entry.deflated = std::move(bytes);
    entry.deflate_state = DeflateState::Ready;
    entry.last_used = next_use();
    bytes_ += entry.deflated->size();
    return true;
}

bool ReportArtifactPayloadCache::insert_pair(
    const ReportArtifactDescriptor &result,
    std::shared_ptr<const LargeByteBuffer> result_bytes,
    const ReportArtifactDescriptor &overview,
    std::shared_ptr<const LargeByteBuffer> overview_bytes) {
    if (!can_hold(result) || !can_hold(overview) || !result_bytes ||
        !overview_bytes || result_bytes->size() != result.size ||
        overview_bytes->size() != overview.size ||
        result.key.kind != ReportArtifactKind::Result ||
        overview.key.kind != ReportArtifactKind::Overview ||
        result.key.sleep_day != overview.key.sleep_day ||
        result.key.source_revision != overview.key.source_revision ||
        result.size + overview.size > byte_budget_) {
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        if (!entries_[i].valid()) continue;
        if (entries_[i].payload.artifact.key == result.key ||
            entries_[i].payload.artifact.key == overview.key) {
            erase(i, false);
        }
    }

    while (bytes_ + result.size + overview.size > byte_budget_ ||
           free_count() < 2) {
        if (!evict_lru()) return false;
    }

    const size_t result_index = find_free();
    if (result_index == SIZE_MAX) return false;
    prepare_entry(entries_[result_index],
                  ReportArtifactPayloadDescriptor::whole(result),
                  std::move(result_bytes));
    bytes_ += entries_[result_index].bytes->size();

    const size_t overview_index = find_free();
    if (overview_index == SIZE_MAX) {
        erase(result_index, false);
        return false;
    }
    prepare_entry(entries_[overview_index],
                  ReportArtifactPayloadDescriptor::whole(overview),
                  std::move(overview_bytes));
    bytes_ += entries_[overview_index].bytes->size();
    return true;
}

bool ReportArtifactPayloadCache::next_deflate_candidate(
    ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> &bytes) const {
    payload = {};
    bytes.reset();

    size_t selected = SIZE_MAX;
    uint64_t newest = 0;
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        const Entry &entry = entries_[i];
        if (!entry.valid() ||
            entry.deflate_state != DeflateState::Pending ||
            entry.last_used < newest) {
            continue;
        }

        selected = i;
        newest = entry.last_used;
    }
    if (selected == SIZE_MAX) return false;

    payload = entries_[selected].payload;
    bytes = entries_[selected].bytes;
    return true;
}

bool ReportArtifactPayloadCache::next_deflate_lookup_candidate(
    ReportArtifactPayloadDescriptor &payload) const {
    payload = {};

    size_t selected = SIZE_MAX;
    uint64_t newest = 0;
    for (size_t i = 0; i < AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY; ++i) {
        const Entry &entry = entries_[i];
        if (!entry.valid() || !entry.bytes ||
            entry.deflate_state != DeflateState::Unchecked ||
            entry.last_used < newest) {
            continue;
        }

        selected = i;
        newest = entry.last_used;
    }
    if (selected == SIZE_MAX) return false;

    payload = entries_[selected].payload;
    return true;
}

bool ReportArtifactPayloadCache::mark_deflate_pending(
    const ReportArtifactPayloadDescriptor &payload) {
    const size_t index = find_exact(payload);
    if (index == SIZE_MAX || !entries_[index].bytes) return false;

    if (entries_[index].deflated) {
        bytes_ -= entries_[index].deflated->size();
    }
    entries_[index].deflated.reset();
    entries_[index].deflate_state = DeflateState::Pending;
    return true;
}

bool ReportArtifactPayloadCache::mark_deflate_unavailable(
    const ReportArtifactPayloadDescriptor &payload) {
    const size_t index = find_exact(payload);
    if (index == SIZE_MAX) return false;

    if (entries_[index].deflated) {
        bytes_ -= entries_[index].deflated->size();
    }
    entries_[index].deflated.reset();
    entries_[index].deflate_state = DeflateState::Unavailable;
    return true;
}

bool ReportArtifactPayloadCache::next_deflate_candidate(
    ReportArtifactDescriptor &artifact,
    std::shared_ptr<const LargeByteBuffer> &bytes) const {
    ReportArtifactPayloadDescriptor payload;
    if (!next_deflate_candidate(payload, bytes) || !payload.is_whole()) {
        artifact = {};
        bytes.reset();
        return false;
    }

    artifact = payload.artifact;
    return true;
}

bool ReportArtifactPayloadCache::complete_deflate(
    const ReportArtifactPayloadDescriptor &payload,
    std::shared_ptr<const LargeByteBuffer> bytes) {
    const size_t index = find_exact(payload);
    if (index == SIZE_MAX) return false;

    Entry &entry = entries_[index];
    if (!entry.bytes || !bytes || bytes->size() >= entry.bytes->size()) {
        entry.deflated.reset();
        entry.deflate_state = DeflateState::Unavailable;
        return true;
    }

    while (bytes_ + bytes->size() > byte_budget_) {
        const size_t evicted = find_lru(index);
        if (evicted == SIZE_MAX) {
            entry.deflated.reset();
            entry.deflate_state = DeflateState::Unavailable;
            return true;
        }
        erase(evicted, true);
    }

    entry.deflated = std::move(bytes);
    entry.deflate_state = DeflateState::Ready;
    entry.last_used = next_use();
    bytes_ += entry.deflated->size();
    return true;
}

bool ReportArtifactPayloadCache::reserve(size_t wanted, size_t excluded) {
    if (wanted > byte_budget_) return false;
    while (bytes_ + wanted > byte_budget_ ||
           (excluded == SIZE_MAX && find_free() == SIZE_MAX)) {
        const size_t evicted = find_lru(excluded);
        if (evicted == SIZE_MAX) return false;
        erase(evicted, true);
    }
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

        const NightCatalogRecord *night = catalog.find(
            entries_[i].payload.artifact.key.sleep_day);
        if (!night || night->source_revision !=
                          entries_[i].payload.artifact.key.source_revision) {
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
