#pragma once

#include "report_oximetry.h"

namespace aircannect {

// Storage receipts are collected before selection, including receipts for a
// different representation of the same ring. No report tiles are needed.
class SleepHqOximetryPlan {
public:
    using Catalog = std::shared_ptr<const ReportOximetryCatalog>;
    using CatalogProvider = Catalog (*)();
    using MemoryEnabled = bool (*)(const char *ble_address);

    static const char *source_id(const ReportOximetryRecording &record,
                                 const ReportOximetryCatalog &catalog, char (&ble)[17]);
    void begin(Catalog catalog, std::string_view preference = "auto");
    const char *next_source();
    void start_uploads();
    bool inspect(const char *path, bool confirmed);
    const ReportOximetryRecording *ring_csv(const char *path);
    bool eligible(const char *path, int64_t now, MemoryEnabled downloads);
    bool collecting() const { return collecting_; }
    int64_t next_deadline() const { return deadline_; }

private:
    struct File {
        const ReportOximetryRecording *record;
        bool published = false, confirmed = false;
    };
    File *find(const char *path);

    Catalog catalog_;
    std::vector<File, LargeAllocator<File>> files_;
    size_t cursor_ = 0;
    bool collecting_ = true;
    bool automatic_ = false;
    int64_t deadline_ = 0;
};

}  // namespace aircannect
