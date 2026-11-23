#pragma once

#include "oximetry_archive.h"
#include "report_oximetry.h"

namespace aircannect {

// Storage receipts are collected before selection, including receipts for a
// different representation of the same ring. No report tiles are needed.
class SleepHqOximetryPlan {
public:
    using Catalog = std::shared_ptr<const ReportOximetryCatalog>;
    using CatalogProvider = Catalog (*)();
    using MemoryEnabled = bool (*)(const char *ble_source_id);

    void begin(Catalog catalog, std::string_view preference = "auto", std::string_view day = {},
               bool prefer_bin = false);
    const char *next_source();
    const char *inventory_day() const { return inventory_day_[0] ? inventory_day_ : nullptr; }
    void start_uploads();
    bool inspect(const char *path, bool confirmed);
    const ReportOximetryRecording *ring_csv(const char *path);
    bool eligible(const char *path, int64_t now, MemoryEnabled downloads);
    bool collecting() const { return collecting_; }
    int64_t next_deadline() const { return deadline_; }

private:
    struct File {
        const ReportOximetryRecording *record;
        OximetryArchiveFile upload = OximetryArchiveFile::Other;
        bool confirmed = false;
    };
    File *find(const char *path, OximetryArchiveFile &kind);

    Catalog catalog_;
    std::vector<File, LargeAllocator<File>> files_;
    size_t cursor_ = 0;
    bool collecting_ = true;
    bool scoped_ = false;
    bool prefer_bin_ = false;
    char inventory_day_[9]{};
    int64_t deadline_ = 0;
};

}  // namespace aircannect
