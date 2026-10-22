#include "report_date_repair.h"

#include <algorithm>
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>

#include "large_allocator.h"
#include "little_endian.h"
#include "night_catalog_builder.h"
#include "night_catalog_file.h"
#include "night_catalog_store_service.h"
#include "report_fallback_artifact.h"
#include "report_signal_store.h"
#include "report_build_checkpoint.h"
#include "string_util.h"

namespace aircannect {
namespace {
using namespace LittleEndian;
constexpr const char *ROOT = "/aircannect/report/date-repair";
constexpr const char *PROGRESS = "/aircannect/report/date-repair/progress.bin";
constexpr size_t CHUNK = 16384;
constexpr size_t MAX_PLAN = 4 * 1024 * 1024;
constexpr size_t JOB_HEADER = 24 + 2 * AC_STORAGE_PATH_MAX;

template<class T> using Items = std::vector<T, LargeAllocator<T>>;

std::string batch_root(uint32_t batch) {
    if (!batch) return ROOT;

    char path[AC_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/b%08x", ROOT, batch);
    return path;
}

std::string numbered(uint32_t batch, const char *group, size_t index) {
    char path[AC_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/%08u.bin", batch_root(batch).c_str(), group,
             static_cast<unsigned>(index));
    return path;
}

bool ends(const std::string &path, const char *suffix) {
    const size_t length = strlen(suffix);
    return path.size() >= length && path.compare(path.size() - length, length, suffix) == 0;
}

struct Job {
    std::string source;
    std::string target;
    uint64_t size = 0;
    std::shared_ptr<const LargeByteBuffer> prefix;
    bool remove = false;
    SleepDayId repair_day;
    SourceRevision repair_revision;
    uint32_t repair_generation = 0;
    uint64_t extra_offset = 0;
    std::shared_ptr<const LargeByteBuffer> extra;
};

struct Candidate {
    SleepDayId day;
    std::shared_ptr<const NightCatalog> catalog;
    Items<std::string> active_paths;
    Items<std::string> report_roots;
    std::shared_ptr<const LargeByteBuffer> fallback;
    ReportFallbackArtifactView fallback_view;
    std::string fallback_path;
    ReportSignalStoreMetadata report;
    std::string report_path;
};
}

struct ReportDateRepairService::Runtime {
    // Storage owners
    StorageReadPort *reads = nullptr;
    StorageAtomicWritePort *writes = nullptr;
    StorageRangeWritePort *ranges = nullptr;
    StorageScanPort *scans = nullptr;
    StorageDeletePort *deletes = nullptr;

    // Accepted sources and the complete batch plan
    ReportDateRepairStatus status;
    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const StorageScanSnapshot> scan;
    Items<Candidate> candidates;
    Items<SleepDayId> retained_sources;
    Items<Job> jobs;
    std::shared_ptr<const LargeByteBuffer> description;
    std::shared_ptr<const LargeByteBuffer> plan;

    // Discovery and durable transaction cursor
    size_t cursor = 0;
    uint32_t journal_jobs = 0;
    uint32_t batch = 0;
    uint8_t save_stage = 0;
    std::shared_ptr<const LargeByteBuffer> completed_progress;
    size_t inspect_cursor = 0;
    bool boot_plan = false;
    bool progress_pending = false;
    bool apply_requested = false;
    bool journal_checked = false;
    bool planned = false;
    size_t patch_cursor = 0;

    // Bounded asynchronous I/O
    OperationTicket scan_ticket, read_ticket, write_ticket, range_ticket;
    StoragePreparedRead prepared;
    std::unique_ptr<LargeByteBuffer> reading;
    size_t read_position = 0;
    uint64_t file_size = 0;
    bool missing = false;
    std::shared_ptr<const LargeByteBuffer> loaded;
    std::shared_ptr<const LargeByteBuffer> writing;

    // One staged copy at a time
    uint64_t copy_offset = 0;
    size_t copy_count = 0;
    uint32_t delete_id = 0;
    enum class CopyPhase { Read, Write, Publish } copy_phase = CopyPhase::Read;

    bool fail(const char *error) {
        if (status.state == ReportDateRepairState::CheckingJournal) status.durable = true;
        copy_cstr(status.error, sizeof(status.error), error);
        status.state = ReportDateRepairState::Failed;
        return false;
    }

    bool complete() {
        status.state = ReportDateRepairState::Complete;
        Items<Candidate>().swap(candidates);
        Items<SleepDayId>().swap(retained_sources);
        Items<Job>().swap(jobs);
        scan.reset();
        catalog.reset();
        plan.reset();
        return true;
    }

    // Reads a bounded prefix, in storage-sized chunks. Accepted data is owned
    // once and decoded by the corresponding format owner, never revalidated.
    bool read(const std::string &path, size_t limit, uint64_t offset = 0) {
        if (!read_ticket.valid() && !prepared.valid()) {
            StorageReadCommand command;
            command.path = path;
            command.offset = offset + read_position;
            command.length = std::min(CHUNK, limit - read_position);
            command.generation = 1;
            const auto submitted = reads->request_read(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) return fail("date_repair_read_rejected"), false;
            read_ticket = submitted.ticket;
            return false;
        }

        if (read_ticket.valid()) {
            StorageReadCompletion completion;
            if (!reads->take_completion(read_ticket, completion)) return false;
            read_ticket = {};
            if (completion.outcome.disposition != OperationDisposition::Succeeded) {
                if (completion.prepared.valid()) reads->release_prepared(completion.prepared);
                if (strcmp(completion.error, "read_not_found") != 0) {
                    fail(completion.error);
                    return false;
                }
                missing = true;
                return true;
            }
            prepared = completion.prepared;
            file_size = completion.file_size;
            if (!reading) {
                const size_t size = offset < file_size
                    ? static_cast<size_t>(std::min<uint64_t>(limit, file_size - offset)) : 0;
                reading = LargeByteBuffer::allocate(size);
                if (!size || !reading) return fail("date_repair_read_size"), false;
            }
        }

        const auto view = reads->view_prepared(prepared);
        if (view.state == PreparedByteReadState::Retry) return false;
        const size_t wanted = std::min(CHUNK, reading->size() - read_position);
        if (!view.valid() || view.length != wanted) return fail("date_repair_short_read"), false;
        memcpy(reading->data() + read_position, view.data, wanted);
        reads->release_prepared(prepared);
        prepared = {};
        read_position += wanted;
        if (read_position != reading->size()) return false;

        loaded = LargeByteBuffer::freeze(std::move(reading));
        read_position = 0;
        return true;
    }

    bool write(const std::string &path, std::shared_ptr<const LargeByteBuffer> bytes,
               const std::string &staged = {}, uint64_t size = 0) {
        if (!write_ticket.valid()) {
            StorageAtomicWriteCommand command;
            command.path = path;
            command.bytes = std::move(bytes);
            command.staged_path = staged;
            command.staged_size = size;
            command.generation = 1;
            const auto submitted = writes->request_write(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) return fail("date_repair_write_rejected"), false;
            writing = command.bytes;
            write_ticket = submitted.ticket;
            return false;
        }

        StorageAtomicWriteCompletion completion;
        if (!writes->take_completion(write_ticket, completion)) return false;

        write_ticket = {};
        const auto expected = writing ? writing->size() : size;
        writing.reset();
        if (completion.outcome.disposition != OperationDisposition::Succeeded ||
            completion.bytes_written != expected) return fail("date_repair_write_failed"), false;
        return true;
    }

    Candidate &candidate(SleepDayId day) {
        for (auto &item : candidates) if (item.day == day) return item;
        candidates.emplace_back();
        candidates.back().day = day;
        return candidates.back();
    }

    void accept_catalog(std::shared_ptr<const NightCatalog> value, const std::string &path = {}) {
        for (size_t i = 0; value && i < value->size(); ++i) {
            const auto &record = *value->record(i);
            if (!record.unresolved_history) continue;
            auto &item = candidate(record.sleep_day);
            if (!path.empty()) item.active_paths.push_back(path);

            if (!item.catalog || !record.sources_external) {
                item.catalog = NightCatalogBuilder::select_night(*value, record.sleep_day);
            }
        }
    }

    bool inspect(const StorageScanEntryView &entry) {
        const std::string path(entry.path);
        const bool fallback = path.compare(0, strlen(REPORT_FALLBACK_ARTIFACT_ROOT),
                                           REPORT_FALLBACK_ARTIFACT_ROOT) == 0;
        const bool metadata = ends(path, "/night.meta") || ends(path, "/clock-conflict.meta");
        const bool sources = ends(path, "/sources.bin") ||
            path.compare(0, strlen("/aircannect/report/v9/history/"),
                         "/aircannect/report/v9/history/") == 0;
        if (!fallback && !metadata && !sources) return true;

        if (!read(path, fallback ? ReportFallbackArtifactCodec::MaxMetadataBytes : 512 * 1024)) return false;
        auto bytes = std::move(loaded);
        if (missing) { missing = false; return fail("date_repair_source_disappeared"), false; }

        if (fallback) {
            ReportFallbackArtifactView view;
            if (!ReportFallbackArtifactCodec::decode_metadata(bytes->data(), bytes->size(), view) ||
                view.info.total_bytes != file_size) return fail("date_repair_fallback_invalid"), false;
            if (view.info.legacy_day_conflict) {
                auto &item = candidate(view.info.sleep_day);
                item.fallback = bytes;
                item.fallback_view = view;
                item.fallback_path = path;
                item.active_paths.push_back(path);
            } else {
                retained_sources.push_back(view.info.sleep_day);
            }
        } else if (metadata) {
            ReportSignalStoreMetadata saved(bytes);
            if (!saved.metadata) return fail("date_repair_metadata_invalid"), false;
            if (saved.view.night.flags & REPORT_SIGNAL_STORE_NIGHT_TIME_UNRESOLVED) {
                auto &item = candidate(saved.view.night.sleep_day);
                item.active_paths.push_back(path);
                char generation[16];
                snprintf(generation, sizeof(generation), "/g%08x/", saved.view.night.generation);
                item.report_roots.push_back(path.substr(0, path.find_last_of('/')) + generation);

                if (!item.report.metadata || ends(path, "/clock-conflict.meta")) {
                    item.report = saved;
                    item.report_path = path;
                }
            }
        } else {
            if (bytes->size() < NightCatalogFileCodec::HeaderBytes || bytes->size() != file_size) {
                return fail("date_repair_catalog_size"), false;
            }
            const auto value = NightCatalogFileCodec::decode(bytes->data(), NightCatalogFileCodec::HeaderBytes,
                bytes->data() + NightCatalogFileCodec::HeaderBytes, bytes->size() - NightCatalogFileCodec::HeaderBytes);
            if (!value) return fail("date_repair_catalog_invalid"), false;
            accept_catalog(value, path);
        }
        return true;
    }

    void add(std::string source, std::string target, uint64_t size,
             std::shared_ptr<const LargeByteBuffer> prefix = {}) {
        Job job;
        job.source = std::move(source);
        job.target = std::move(target);
        job.size = size;
        job.prefix = std::move(prefix);
        jobs.push_back(std::move(job));
    }

    std::shared_ptr<const NightCatalog> source_catalog(Candidate &item) {
        if (item.catalog) return item.catalog;
        NightCatalogSummaryInput summary;
        Items<NightCatalogTimeRange> sessions;
        summary.sleep_day = item.day;
        summary.identity = 1;
        summary.time_conflict = true;
        summary.timezone_offset_valid = true;
        if (item.fallback) {
            const auto &view = item.fallback_view;
            summary.day_start_ms = view.info.day_start_ms;
            summary.day_end_ms = view.info.day_end_ms;
            summary.timezone_offset_minutes = view.info.timezone_offset_minutes;
            sessions.resize(view.info.session_count);
            for (size_t i = 0; i < sessions.size(); ++i) view.session(i, sessions[i]);
        } else if (item.report.metadata) {
            const auto &view = item.report.view;
            summary.day_start_ms = view.night.day_start_ms;
            summary.day_end_ms = view.night.day_end_ms;
            summary.timezone_offset_minutes = view.night.timezone_offset_minutes;
            sessions.resize(view.night.session_count);
            for (size_t i = 0; i < sessions.size(); ++i) view.session(i, sessions[i]);
        } else return {};
        summary.sessions = sessions.data();
        summary.session_count = sessions.size();
        NightCatalogBuildInput input;
        input.summary_records = &summary;
        input.summary_record_count = 1;
        return NightCatalogBuilder::build(input);
    }

    bool plan_repairs() {
        auto updated = catalog;
        std::string text;

        // Remove old unresolved entries together before inserting corrected days.
        // This avoids overwriting a source in a chain such as 20 -> 19 -> 18.
        for (const auto &item : candidates) {
            const auto *old = updated->find(item.day);
            if (old && old->unresolved_history) updated = NightCatalogBuilder::without_night(*updated, item.day);
            if (!updated) return fail("date_repair_catalog_alloc");
        }

        uint32_t generation = 1;
        StorageScanEntryView entry;
        for (size_t i = 0; i < scan->size(); ++i) {
            scan->entry(i, entry);
            const char *g = strstr(entry.path, "/g");
            unsigned value = 0;
            if (g && sscanf(g, "/g%8x/", &value) == 1) generation = std::max(generation, uint32_t(value));
        }
        if (generation == UINT32_MAX) return fail("date_repair_generation_exhausted");
        ++generation;

        for (auto &item : candidates) {
            const auto old = source_catalog(item);
            auto repaired = old ? NightCatalogBuilder::repair_date(*old, item.day) : nullptr;
            if (!repaired) return fail("date_repair_source_unresolved");
            if (repaired->record(0)->sources_external && !item.fallback) {
                return fail("date_repair_sources_missing");
            }

            const auto day = repaired->record(0)->sleep_day;
            const auto *current = updated->find(day);
            const bool occupied_file = std::find(retained_sources.begin(), retained_sources.end(), day) != retained_sources.end();
            const bool collision = occupied_file || (current && !current->unresolved_history &&
                (current->source_flags & (NIGHT_CATALOG_SOURCE_EDF | NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK |
                                          NIGHT_CATALOG_SOURCE_LOCAL_HISTORY)));
            char from[9], to[9], target[AC_STORAGE_PATH_MAX];
            item.day.format_yyyymmdd(from, sizeof(from));
            day.format_yyyymmdd(to, sizeof(to));
            text += std::string(from) + " -> " + to + (collision ? " archive; existing source retained\n" : " restore\n");

            if (item.fallback) {
                auto fixed = ReportFallbackArtifactCodec::repair_date(item.fallback->data(),
                    item.fallback_view.info.metadata_bytes, item.fallback_view.info);
                if (!fixed) return fail("date_repair_fallback_patch");
                report_fallback_artifact_path(day, target, sizeof(target));
                add(item.fallback_path, collision ? "" : target, item.fallback_view.info.total_bytes, fixed);
                if (!collision) {
                    ReportFallbackArtifactView view;
                    if (!ReportFallbackArtifactCodec::decode_metadata(fixed->data(), fixed->size(), view)) {
                        return fail("date_repair_fallback_patch_invalid");
                    }
                    repaired = NightCatalogBuilder::restore_fallback(*repaired, target, view);
                    if (!repaired) return fail("date_repair_fallback_restore");
                }
            }

            if (collision) {
                if (item.report.metadata) {
                    add(item.report_path, "", item.report.metadata->size());
                    char base[AC_STORAGE_PATH_MAX];
                    snprintf(base, sizeof(base), "%s/%s/g%08x/", REPORT_SIGNAL_STORE_ROOT,
                             from, item.report.view.night.generation);
                    for (size_t i = 0; i < scan->size(); ++i) {
                        scan->entry(i, entry);
                        if (!entry.directory && strncmp(entry.path, base, strlen(base)) == 0) add(entry.path, "", entry.size);
                    }
                }
                ++status.archived;
                continue;
            }

            const auto &record = *repaired->record(0);
            NightCatalogFileCodec::sources_path(day, target, sizeof(target));
            auto source_bytes = NightCatalogFileCodec::encode(*repaired);
            if (!source_bytes) return fail("date_repair_sources_encode");
            add("", target, source_bytes->size(), source_bytes);

            if (item.report.metadata) {
                const auto fixed = ReportSignalStoreNightCodec::repair_date(item.report, day,
                    record.source_revision, generation);
                if (!fixed.metadata) return fail("date_repair_report_patch");
                char old_base[AC_STORAGE_PATH_MAX], new_base[AC_STORAGE_PATH_MAX];
                snprintf(old_base, sizeof(old_base), "%s/%s/g%08x/", REPORT_SIGNAL_STORE_ROOT,
                         from, item.report.view.night.generation);
                snprintf(new_base, sizeof(new_base), "%s/%s/g%08x/", REPORT_SIGNAL_STORE_ROOT, to, generation);

                for (size_t i = 0; i < scan->size(); ++i) {
                    scan->entry(i, entry);
                    if (entry.directory || strncmp(entry.path, old_base, strlen(old_base)) != 0) continue;
                    const std::string suffix(entry.path + strlen(old_base));
                    add(entry.path, std::string(new_base) + suffix, entry.size);
                    auto &job = jobs.back();
                    job.repair_day = day;
                    job.repair_revision = record.source_revision;
                    job.repair_generation = generation;

                    for (size_t t = 0; t < fixed.view.night.track_count; ++t) {
                        ReportSignalStoreTrack track;
                        fixed.view.track(t, track);
                        for (auto level : {ReportSignalStoreLevel::Raw,
                             ReportSignalStoreLevel::OneSecond, ReportSignalStoreLevel::TenSeconds}) {
                            char signal[AC_STORAGE_PATH_MAX];
                            if (!report_signal_store_signal_path(track, level, signal, sizeof(signal)) ||
                                job.target != signal) continue;

                            job.prefix = ReportSignalStoreFileCodec::encode_header(track, level);
                            if (!job.prefix) return fail("date_repair_signal_header");
                        }
                    }
                }
                report_signal_store_night_path(day, target, sizeof(target));
                add(item.report_path, target, fixed.metadata->size(), fixed.metadata);
            }

            updated = NightCatalogBuilder::upsert_night(*updated, *repaired, day);
            if (!updated) return fail("date_repair_catalog_merge");
            ++status.restored;
        }

        // Preserve every accepted copy, including metadata not selected for
        // restoration and the generations it protects, before retiring pins.
        for (const auto &item : candidates) {
            for (size_t i = 0; i < scan->size(); ++i) {
                scan->entry(i, entry);
                if (entry.directory) continue;

                bool original = std::find(item.active_paths.begin(), item.active_paths.end(),
                                          entry.path) != item.active_paths.end();

                for (const auto &root : item.report_roots) {
                    if (strncmp(entry.path, root.c_str(), root.size()) == 0) original = true;
                }
                if (!original) continue;

                const bool saved = std::any_of(jobs.begin(), jobs.end(),
                    [&](const Job &job) { return job.source == entry.path; });

                if (!saved) add(entry.path, "", entry.size);
            }
        }

        // Active discovery must not find repaired history again. A path used
        // as a destination in this same chain already receives corrected data.
        for (const auto &item : candidates) {
            for (const auto &path : item.active_paths) {
                const bool replaced = std::any_of(jobs.begin(), jobs.end(),
                    [&](const Job &job) { return job.target == path; });

                if (replaced) continue;
                Job remove;
                remove.target = path;
                remove.remove = true;
                jobs.push_back(std::move(remove));
            }
        }
        auto index = NightCatalogBuilder::index(*updated);
        auto bytes = index ? NightCatalogFileCodec::encode(*index) : nullptr;
        if (!bytes) return fail("date_repair_index_encode");

        add(NIGHT_CATALOG_STORE_PATH, NIGHT_CATALOG_STORE_PATH, bytes->size(), bytes);
        description = LargeByteBuffer::copy_and_freeze(reinterpret_cast<const uint8_t *>(text.data()), text.size());
        status.candidates = candidates.size();
        planned = true;
        return true;
    }

    bool prepare_copy_headers() {
        if (patch_cursor == jobs.size()) {
            status.state = ReportDateRepairState::Preview;
            return true;
        }

        auto &job = jobs[patch_cursor];
        const bool checkpoint = job.repair_day.valid() && ends(job.source, ".state");
        const bool events = job.repair_day.valid() && ends(job.source, "/events.evt");
        if (!checkpoint && !events) { ++patch_cursor; return true; }
        if (!read(job.source, checkpoint ? ReportBuildCheckpointCodec::MaxBytes : MAX_PLAN)) return false;
        if (missing || !loaded || loaded->size() != job.size) return fail("date_repair_report_file_missing");

        if (checkpoint) {
            ReportBuildCheckpointInput accepted;
            accepted.bytes = loaded;
            if (!ReportBuildCheckpointCodec::decode(loaded->data(), loaded->size(), accepted.view)) {
                return fail("date_repair_checkpoint_invalid");
            }
            const auto fixed = ReportBuildCheckpointCodec::repair_identity(accepted,
                job.repair_day, job.repair_revision, job.repair_generation);
            if (!fixed) return fail("date_repair_checkpoint_patch");
            job.prefix = LargeByteBuffer::copy_and_freeze(fixed->data(), ReportBuildCheckpointCodec::HeaderBytes);
            if (accepted.view.events_size) {
                job.extra_offset = accepted.view.events - loaded->data();
                job.extra = LargeByteBuffer::copy_and_freeze(fixed->data() + job.extra_offset,
                                                            ReportSignalStoreEventCodec::HeaderBytes);
                if (!job.extra) return fail("date_repair_event_patch_alloc");
            }
        } else {
            ReportSignalStoreEventFileView accepted;
            if (!ReportSignalStoreEventCodec::inspect(loaded->data(), loaded->size(), accepted)) {
                return fail("date_repair_events_invalid");
            }
            job.prefix = ReportSignalStoreEventCodec::repair_identity(accepted,
                job.repair_day, job.repair_revision, job.repair_generation);
        }
        if (!job.prefix) return fail("date_repair_patch_alloc");
        loaded.reset();
        ++patch_cursor;
        return true;
    }

    bool encode_plan() {
        size_t size = 16;
        for (const auto &job : jobs) {
            size += JOB_HEADER + (job.prefix ? job.prefix->size() : 0) + (job.extra ? job.extra->size() : 0);
            if (size > MAX_PLAN) return fail("date_repair_plan_too_large");
        }
        auto bytes = LargeByteBuffer::allocate(size);
        if (!bytes) return fail("date_repair_plan_alloc");

        memset(bytes->data(), 0, size);
        memcpy(bytes->data(), "ACDATE01", 8);
        put_le32(bytes->data() + 8, jobs.size());
        put_le32(bytes->data() + 12, size);
        size_t offset = 16;
        for (const auto &job : jobs) {
            auto *out = bytes->data() + offset;
            put_le64(out, job.remove ? UINT64_MAX : job.size);
            put_le32(out + 8, job.prefix ? job.prefix->size() : 0);
            put_le64(out + 12, job.extra_offset);
            put_le32(out + 20, job.extra ? job.extra->size() : 0);
            copy_cstr(reinterpret_cast<char *>(out + 24), AC_STORAGE_PATH_MAX, job.source.c_str());
            copy_cstr(reinterpret_cast<char *>(out + 24 + AC_STORAGE_PATH_MAX), AC_STORAGE_PATH_MAX, job.target.c_str());
            offset += JOB_HEADER;
            if (job.prefix) {
                memcpy(bytes->data() + offset, job.prefix->data(), job.prefix->size());
                offset += job.prefix->size();
            }
            if (job.extra) {
                memcpy(bytes->data() + offset, job.extra->data(), job.extra->size());
                offset += job.extra->size();
            }
        }
        plan = LargeByteBuffer::freeze(std::move(bytes));
        return true;
    }

    bool decode_plan() {
        if (!loaded || loaded->size() < 16 || memcmp(loaded->data(), "ACDATE01", 8) ||
            get_le32(loaded->data() + 12) != loaded->size()) return fail("date_repair_journal_invalid");
        jobs.clear();
        const size_t count = get_le32(loaded->data() + 8);
        if (count != journal_jobs) return fail("date_repair_journal_count"), false;

        size_t offset = 16;
        for (size_t i = 0; i < count; ++i) {
            if (offset > loaded->size() || JOB_HEADER > loaded->size() - offset) return fail("date_repair_journal_short");
            const auto *in = loaded->data() + offset;
            const size_t length = get_le32(in + 8);
            const auto extra_size = get_le32(in + 20);
            const char *source = reinterpret_cast<const char *>(in + 24);
            const char *target = source + AC_STORAGE_PATH_MAX;
            if (!memchr(source, 0, AC_STORAGE_PATH_MAX) || !memchr(target, 0, AC_STORAGE_PATH_MAX) ||
                (source[0] && (!storage_user_path_valid(source) || strncmp(source, "/aircannect/report/", 18))) ||
                (target[0] && (!storage_user_path_valid(target) || strncmp(target, "/aircannect/report/", 18)))) {
                return fail("date_repair_journal_path");
            }
            Job job;
            job.source = source;
            job.target = target;
            job.size = get_le64(in);
            job.remove = job.size == UINT64_MAX;
            job.extra_offset = get_le64(in + 12);
            offset += JOB_HEADER;
            if (length > loaded->size() - offset || (!job.remove && length > job.size)) return fail("date_repair_journal_prefix");
            if (length) job.prefix = LargeByteBuffer::copy_and_freeze(loaded->data() + offset, length);
            if (length && !job.prefix) return fail("date_repair_journal_alloc");
            offset += length;
            if (extra_size > loaded->size() - offset || job.extra_offset > job.size ||
                extra_size > job.size - job.extra_offset) return fail("date_repair_journal_extra");
            if (extra_size) job.extra = LargeByteBuffer::copy_and_freeze(loaded->data() + offset, extra_size);
            if (extra_size && !job.extra) return fail("date_repair_journal_alloc");
            offset += extra_size;
            jobs.push_back(std::move(job));
        }

        loaded.reset();
        if (offset != file_size || cursor > jobs.size()) return fail("date_repair_journal_length"), false;
        return true;
    }

    bool save_progress(uint32_t phase) {
        auto bytes = LargeByteBuffer::allocate(20);
        if (!bytes) return fail("date_repair_progress_alloc"), false;
        memcpy(bytes->data(), "ACDR", 4);
        put_le32(bytes->data() + 4, phase);
        put_le32(bytes->data() + 8, cursor);
        put_le32(bytes->data() + 12, jobs.size());
        put_le32(bytes->data() + 16, batch);
        const auto progress = LargeByteBuffer::freeze(std::move(bytes));
        if (!write(PROGRESS, progress)) return false;
        if (phase == 2) completed_progress = progress;
        return true;
    }

    bool copy_job(bool backup) {
        const auto &job = jobs[cursor];
        const std::string archived = numbered(batch, "originals", cursor);
        const std::string target = backup ? archived : job.target;
        if ((backup && job.source.empty()) || (!backup && job.target.empty())) return true;
        if (!backup && job.remove) {
            if (!delete_id) {
                if (!read(target, 1)) return false;
                loaded.reset();
                if (missing) { missing = false; return true; }
                const auto slash = target.find_last_of('/');
                const auto base = target.substr(0, slash), name = target.substr(slash + 1);
                const char *names[] = {name.c_str()};
                char error[AC_STORAGE_ERROR_MAX] = {};
                if (!deletes->start_selected(base.c_str(), names, 1, &delete_id, error, sizeof(error))) {
                    if (deletes->active()) return false;
                    return fail(error), false;
                }
                return false;
            }
            StorageDeleteStatus state;
            if (!deletes->status(state) || state.state == StorageDeleteState::Deleting) return false;
            delete_id = 0;
            return state.state == StorageDeleteState::Done ? true : (fail("date_repair_remove_failed"), false);
        }

        const auto prefix = backup ? nullptr : job.prefix;
        const std::string staged = numbered(batch, "pending", cursor);
        if (copy_phase == CopyPhase::Read) {
            if (prefix && copy_offset < prefix->size()) {
                const size_t count = std::min(CHUNK, prefix->size() - static_cast<size_t>(copy_offset));
                loaded = LargeByteBuffer::copy_and_freeze(prefix->data() + copy_offset, count);
                if (!loaded) return fail("date_repair_copy_alloc"), false;
            } else {
                if (!read(backup ? job.source : archived, CHUNK, copy_offset)) return false;
                if (missing) { missing = false; return fail("date_repair_original_missing"), false; }
            }
            copy_count = loaded->size();
            if (!backup && job.extra && copy_offset < job.extra_offset + job.extra->size() &&
                copy_offset + copy_count > job.extra_offset) {
                auto patched = LargeByteBuffer::allocate(copy_count);
                if (!patched) return fail("date_repair_extra_alloc"), false;
                memcpy(patched->data(), loaded->data(), copy_count);
                const uint64_t from = std::max(copy_offset, job.extra_offset);
                const uint64_t to = std::min(copy_offset + copy_count, job.extra_offset + job.extra->size());
                memcpy(patched->data() + from - copy_offset, job.extra->data() + from - job.extra_offset, to - from);
                loaded = LargeByteBuffer::freeze(std::move(patched));
            }
            copy_phase = CopyPhase::Write;
        }
        if (copy_phase == CopyPhase::Write) {
            if (!range_ticket.valid()) {
                StorageRangeWriteCommand command;
                command.path = staged;
                command.bytes = loaded;
                command.offset = copy_offset;
                command.truncate = copy_offset == 0;
                command.generation = 1;
                const auto submitted = ranges->request_write(command);
                if (submitted.admission == OperationAdmission::Busy) return false;
                if (!submitted.accepted()) return fail("date_repair_copy_rejected"), false;
                range_ticket = submitted.ticket;
                return false;
            }
            StorageRangeWriteCompletion completion;
            if (!ranges->take_completion(range_ticket, completion)) return false;
            range_ticket = {};
            loaded.reset();
            if (completion.outcome.disposition != OperationDisposition::Succeeded ||
                completion.bytes_written != copy_count) return fail("date_repair_copy_failed"), false;
            copy_offset += copy_count;
            const uint64_t total = backup ? file_size : job.size;
            if (copy_offset < total) { copy_phase = CopyPhase::Read; return false; }
            if (copy_offset != total) return fail("date_repair_copy_length"), false;
            copy_phase = CopyPhase::Publish;
        }
        if (!write(target, {}, staged, copy_offset)) return false;
        copy_offset = 0;
        copy_phase = CopyPhase::Read;
        return true;
    }

    bool poll() {
        using State = ReportDateRepairState;
        if (status.state == State::CheckingJournal) {
            if (!read(boot_plan ? batch_root(batch) + "/plan.bin" : PROGRESS,
                      boot_plan ? MAX_PLAN : 20)) return false;
            if (boot_plan) {
                if (missing || !decode_plan()) return fail("date_repair_plan_unavailable");
                status.state = apply_requested ? State::Installing : State::BackingUp;
                return true;
            }
            journal_checked = true;
            if (missing) { missing = false; status.state = State::Idle; return true; }
            status.durable = true;
            if (!loaded || (file_size != 16 && file_size != 20) || loaded->size() != file_size ||
                memcmp(loaded->data(), "ACDR", 4)) return fail("date_repair_progress_invalid");
            const auto phase = get_le32(loaded->data() + 4);
            cursor = get_le32(loaded->data() + 8);
            journal_jobs = get_le32(loaded->data() + 12);
            batch = file_size == 20 ? get_le32(loaded->data() + 16) : 0;
            status.completed_files = cursor;
            if (phase == 2) completed_progress = loaded;
            loaded.reset();
            if (cursor > journal_jobs || (phase == 2 && cursor != journal_jobs)) return fail("date_repair_progress_cursor");
            if (phase == 2) return complete();
            if (phase > 2) return fail("date_repair_progress_phase");
            status.durable = true;
            apply_requested = phase == 1;
            boot_plan = true;
            return true;
        }
        if (status.state == State::Scanning) {
            if (!scan_ticket.valid()) {
                const StorageScanRoot roots[] = {{"/aircannect/report/v9/nights", true},
                    {"/aircannect/report/v9/history", true}, {REPORT_FALLBACK_ARTIFACT_ROOT, true}};
                const auto submitted = scans->request_scan({roots, 3, false, 1});
                if (submitted.admission == OperationAdmission::Busy) return false;
                if (!submitted.accepted()) return fail("date_repair_scan_rejected");
                scan_ticket = submitted.ticket;
                return true;
            }
            StorageScanCompletion completion;
            if (!scans->take_completion(scan_ticket, completion)) return false;
            scan_ticket = {};
            if (completion.outcome.disposition != OperationDisposition::Succeeded || !completion.snapshot) return fail("date_repair_scan_failed");
            scan = completion.snapshot;
            status.state = State::Planning;
            return true;
        }
        if (status.state == State::Planning) {
            if (planned) return prepare_copy_headers();
            if (inspect_cursor < scan->size()) {
                StorageScanEntryView entry;
                scan->entry(inspect_cursor, entry);
                if (!entry.directory && !inspect(entry)) return false;
                ++inspect_cursor;
                return true;
            }
            return plan_repairs();
        }
        if (status.state == State::SavingPlan) {
            // The old active journal remains authoritative until the new plan
            // is durable. No original bytes are copied before this switch.
            if (save_stage == 0) {
                if (completed_progress) {
                    const uint32_t previous_batch = completed_progress->size() == 20
                        ? get_le32(completed_progress->data() + 16) : 0;
                    if (!write(batch_root(previous_batch) + "/complete.bin", completed_progress)) return false;
                }
                save_stage = 1;
            }
            if (save_stage == 1) {
                if (!write(batch_root(batch) + "/plan.bin", plan)) return false;
                save_stage = 2;
            }
            status.durable = true;
            if (!save_progress(0)) return false;
            progress_pending = false;
            status.state = State::BackingUp;
            return true;
        }
        if (status.state == State::BackingUp || status.state == State::Installing) {
            const bool backup = status.state == State::BackingUp;
            if (progress_pending) {
                if (!save_progress(backup ? 0 : 1)) return false;
                progress_pending = false;
            }
            if (cursor == jobs.size()) {
                if (backup) {
                    cursor = 0;
                    status.completed_files = 0;
                    status.state = State::Installing;
                    progress_pending = true;
                } else {
                    if (!save_progress(2)) return false;
                    return complete();
                }
                return true;
            }
            if (!copy_job(backup)) return false;
            ++cursor;
            status.completed_files = cursor;
            progress_pending = true;
            return true;
        }
        return false;
    }
};

ReportDateRepairService::ReportDateRepairService() : runtime_(new Runtime) {}
ReportDateRepairService::~ReportDateRepairService() { cancel(); }

void ReportDateRepairService::begin(StorageReadPort &reads, StorageAtomicWritePort &writes,
    StorageRangeWritePort &ranges, StorageScanPort &scans, StorageDeletePort &deletes) {
    auto &r = *runtime_;
    r.reads = &reads;
    r.writes = &writes;
    r.ranges = &ranges;
    r.scans = &scans;
    r.deletes = &deletes;
    r.status.state = ReportDateRepairState::CheckingJournal;
}

bool ReportDateRepairService::start(std::shared_ptr<const NightCatalog> catalog) {
    auto &r = *runtime_;
    if (!catalog || active() || (r.status.durable && r.status.state != ReportDateRepairState::Complete) ||
        !r.journal_checked || !r.deletes) return false;

    cancel();
    r.reading.reset();
    r.loaded.reset();
    r.read_position = 0;
    r.missing = false;
    r.catalog = std::move(catalog);
    r.candidates.clear();
    r.jobs.clear();
    r.description.reset();
    r.retained_sources.clear();
    r.cursor = r.inspect_cursor = 0;
    r.patch_cursor = 0;
    r.planned = false;
    r.progress_pending = false;
    r.copy_offset = 0;
    r.copy_phase = Runtime::CopyPhase::Read;
    r.status = {};
    r.accept_catalog(r.catalog);
    r.status.state = ReportDateRepairState::Scanning;
    return true;
}

bool ReportDateRepairService::apply() {
    auto &r = *runtime_;
    if (r.status.state != ReportDateRepairState::Preview || !r.status.candidates ||
        r.batch == UINT32_MAX || !r.encode_plan()) return false;
    ++r.batch;
    r.save_stage = 0;
    r.cursor = 0;
    r.status.state = ReportDateRepairState::SavingPlan;
    return true;
}

bool ReportDateRepairService::poll() {
    try { return runtime_->poll(); }
    catch (const std::bad_alloc &) { return runtime_->fail("date_repair_alloc"); }
}

bool ReportDateRepairService::active() const {
    const auto state = runtime_->status.state;
    return state != ReportDateRepairState::Idle && state != ReportDateRepairState::Preview &&
           state != ReportDateRepairState::Complete && state != ReportDateRepairState::Failed;
}

const ReportDateRepairStatus &ReportDateRepairService::status() const { return runtime_->status; }
std::shared_ptr<const LargeByteBuffer> ReportDateRepairService::preview() const { return runtime_->description; }

void ReportDateRepairService::cancel() {
    auto &r = *runtime_;
    if (r.read_ticket.valid()) r.reads->abandon(r.read_ticket);
    if (r.prepared.valid()) r.reads->release_prepared(r.prepared);
    if (r.write_ticket.valid()) r.writes->abandon(r.write_ticket);
    if (r.range_ticket.valid()) r.ranges->abandon(r.range_ticket);
    if (r.scan_ticket.valid()) r.scans->abandon(r.scan_ticket);
    if (r.delete_id) r.deletes->cancel(r.delete_id);
    r.read_ticket = {};
    r.prepared = {};
    r.write_ticket = {};
    r.range_ticket = {};
    r.scan_ticket = {};
    r.delete_id = 0;
}
} // namespace aircannect
