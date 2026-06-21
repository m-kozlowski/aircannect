#include "storage_export_planner.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_manager.h"
#include "storage_path.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t EXPORT_WALK_MAX_DEPTH = 16;
static constexpr uint32_t EXPORT_STEP_BUDGET = 16;

const char *const SLEEPHQ_ROOT_METADATA_FILES[] = {
    "/Identification.json",
    "/Identification.crc",
    "/STR.edf",
};

void set_error(char *out, size_t out_size, const char *error) {
    copy_cstr(out, out_size, error ? error : "planner_error");
}

}  // namespace

StorageExportPlanner::~StorageExportPlanner() {
    reset();
}

bool StorageExportPlanner::begin(const StorageExportPlannerConfig &config,
                                 char *error_out,
                                 size_t error_out_size) {
    reset();
    config_ = config;
    state_dir_[0] = '\0';
    latest_datalog_day_[0] = '\0';
    if (config.state_dir) {
        copy_cstr(state_dir_, sizeof(state_dir_), config.state_dir);
        config_.state_dir = state_dir_;
    }
    if (config.latest_datalog_day) {
        copy_cstr(latest_datalog_day_, sizeof(latest_datalog_day_),
                  config.latest_datalog_day);
        config_.latest_datalog_day = latest_datalog_day_;
    }
    if (config_.require_pending_datalog_file && !config_.state_cache) {
        set_error(error_out, error_out_size, "planner_state_cache_missing");
        return false;
    }
    started_ = true;
    return true;
}

void StorageExportPlanner::reset() {
    close_walk();
    release_walk_stack();
    if (datalog_scan_opened_) {
        Storage::Guard guard;
        datalog_scan_dir_.close();
    }
    datalog_scan_opened_ = false;
    datalog_scan_complete_ = false;
    datalog_scan_next_index_ = 0;
    if (datalog_days_) {
        for (size_t i = 0; i < datalog_day_capacity_; ++i) {
            datalog_days_[i].~DatalogDay();
        }
        Memory::free(datalog_days_);
    }
    datalog_days_ = nullptr;
    datalog_day_count_ = 0;
    datalog_day_capacity_ = 0;
    datalog_day_index_ = 0;
    root_index_ = 0;
    day_active_ = false;
    day_root_index_ = 0;
    day_path_[0] = '\0';
    day_name_[0] = '\0';
    state_dir_[0] = '\0';
    latest_datalog_day_[0] = '\0';
    config_ = StorageExportPlannerConfig();
    started_ = false;
}

bool StorageExportPlanner::ensure_walk_stack(char *error_out,
                                             size_t error_out_size) {
    if (walk_stack_) return true;
    walk_capacity_ = EXPORT_WALK_MAX_DEPTH;
    walk_stack_ = static_cast<WalkFrame *>(
        Memory::alloc_large(sizeof(WalkFrame) * walk_capacity_, true));
    if (!walk_stack_) {
        set_error(error_out, error_out_size, "walk_alloc");
        return false;
    }
    for (size_t i = 0; i < walk_capacity_; ++i) {
        new (&walk_stack_[i]) WalkFrame();
    }
    return true;
}

void StorageExportPlanner::close_walk() {
    if (!walk_stack_) return;
    for (size_t i = 0; i < walk_depth_; ++i) {
        if (walk_stack_[i].opened) {
            Storage::Guard guard;
            walk_stack_[i].dir.close();
            walk_stack_[i].opened = false;
        }
    }
    walk_depth_ = 0;
}

void StorageExportPlanner::release_walk_stack() {
    close_walk();
    if (!walk_stack_) return;
    for (size_t i = 0; i < walk_capacity_; ++i) {
        walk_stack_[i].~WalkFrame();
    }
    Memory::free(walk_stack_);
    walk_stack_ = nullptr;
    walk_capacity_ = 0;
}

bool StorageExportPlanner::push_dir(const char *path,
                                    char *error_out,
                                    size_t error_out_size) {
    if (!ensure_walk_stack(error_out, error_out_size)) return false;
    if (walk_depth_ >= walk_capacity_) {
        set_error(error_out, error_out_size, "max_depth");
        return false;
    }
    WalkFrame &frame = walk_stack_[walk_depth_++];
    frame = WalkFrame();
    copy_cstr(frame.path, sizeof(frame.path), path);
    return true;
}

bool StorageExportPlanner::ensure_dir_open(WalkFrame &frame,
                                           char *error_out,
                                           size_t error_out_size) {
    if (frame.opened) return true;
    Storage::Guard guard;
    frame.dir = Storage::open(frame.path, "r");
    if (!frame.dir) {
        set_error(error_out, error_out_size, "local_not_found");
        return false;
    }
    if (!frame.dir.isDirectory()) {
        frame.dir.close();
        set_error(error_out, error_out_size, "local_not_directory");
        return false;
    }
    if (!storage_skip_dir_children(frame.dir, frame.next_index)) {
        frame.dir.close();
        set_error(error_out, error_out_size, "walk_resume_failed");
        return false;
    }
    frame.opened = true;
    return true;
}

bool StorageExportPlanner::state_contains(const char *state_path,
                                          const char *path,
                                          uint64_t size,
                                          uint64_t mtime) {
    return config_.state_cache &&
           config_.state_cache->contains(state_path, path, size, mtime);
}

bool StorageExportPlanner::build_file_item(const char *path,
                                           bool force_export,
                                           StorageExportPlannerItem &out,
                                           char *error_out,
                                           size_t error_out_size) {
    out = StorageExportPlannerItem();
    out.kind = StorageExportPlannerItemKind::File;
    out.force_export = force_export;
    copy_cstr(out.path, sizeof(out.path), path);
    if (day_name_[0]) copy_cstr(out.datalog_day, sizeof(out.datalog_day),
                                day_name_);

    out.info = storage_stat_local_node(path);
    if (!out.info.exists || out.info.is_dir) return false;

    if (state_dir_[0]) {
        if (!storage_export_build_state_path(state_dir_,
                                             path,
                                             out.state_path,
                                             sizeof(out.state_path),
                                             &out.state_write_mode)) {
            set_error(error_out, error_out_size, "state_path_failed");
            return false;
        }
        out.local_state_complete = state_contains(out.state_path,
                                                  path,
                                                  out.info.size,
                                                  out.info.mtime);
    }
    return true;
}

bool StorageExportPlanner::datalog_day_finalized(const char *day) const {
    return day && latest_datalog_day_[0] && strcmp(day, latest_datalog_day_) < 0;
}

bool StorageExportPlanner::datalog_day_done(const char *day) const {
    if (!state_dir_[0] || !day) return false;
    char path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_export_build_done_path(state_dir_, day, path, sizeof(path))) {
        return false;
    }
    const StorageLocalNodeInfo info = storage_stat_local_node(path);
    return info.exists && !info.is_dir;
}

bool StorageExportPlanner::datalog_day_has_pending_files(
    const DatalogDay &day,
    char *error_out,
    size_t error_out_size) {
    if (!day.path[0]) return false;
    File dir;
    {
        Storage::Guard guard;
        dir = Storage::open(day.path, "r");
    }
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            Storage::Guard guard;
            dir.close();
        }
        return false;
    }

    bool pending = false;
    for (;;) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir, child)) break;
        if (child.is_dir) continue;

        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(day.path,
                                       child.name,
                                       child_path,
                                       sizeof(child_path)) ||
            !storage_user_path_valid(child_path)) {
            continue;
        }

        StorageExportPlannerItem item;
        if (!build_file_item(child_path, false, item, error_out,
                             error_out_size)) {
            continue;
        }
        if (!item.local_state_complete) {
            pending = true;
            break;
        }
    }
    {
        Storage::Guard guard;
        dir.close();
    }
    return pending;
}

StorageExportPlannerResult StorageExportPlanner::next(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size) {
    if (!started_) {
        set_error(error_out, error_out_size, "planner_not_started");
        return StorageExportPlannerResult::Error;
    }
    uint32_t budget = EXPORT_STEP_BUDGET;
    if (config_.scope == StorageExportPlannerScope::SleepHq) {
        return next_sleep_hq(out, error_out, error_out_size, budget);
    }
    return next_full_card(out, error_out, error_out_size, budget);
}

StorageExportPlannerResult StorageExportPlanner::next_full_card(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size,
    uint32_t &budget) {
    while (budget > 0) {
        if (walk_depth_ == 0) {
            while (root_index_ < storage_export_root_count() && budget > 0) {
                budget--;
                const StorageExportRoot &root =
                    storage_export_root_at(root_index_++);
                const StorageLocalNodeInfo info =
                    storage_stat_local_node(root.path);
                if (!info.exists) continue;
                if (info.is_dir) {
                    if (root.recursive &&
                        !push_dir(root.path, error_out, error_out_size)) {
                        return StorageExportPlannerResult::Error;
                    }
                    if (root.recursive) break;
                    continue;
                }
                if (build_file_item(root.path, false, out, error_out,
                                    error_out_size)) {
                    return StorageExportPlannerResult::Item;
                }
                if (error_out && error_out[0]) {
                    return StorageExportPlannerResult::Error;
                }
            }
            if (walk_depth_ == 0 &&
                root_index_ >= storage_export_root_count()) {
                return StorageExportPlannerResult::Done;
            }
        }

        StorageExportPlannerResult result =
            next_walk_item(out, error_out, error_out_size, budget);
        if (result != StorageExportPlannerResult::Yield) return result;
    }
    return StorageExportPlannerResult::Yield;
}

StorageExportPlannerResult StorageExportPlanner::next_walk_item(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size,
    uint32_t &budget) {
    while (walk_depth_ > 0 && budget > 0) {
        WalkFrame &frame = walk_stack_[walk_depth_ - 1];
        if (!ensure_dir_open(frame, error_out, error_out_size)) {
            return StorageExportPlannerResult::Error;
        }
        StorageDirChild child;
        if (!storage_read_next_dir_child(frame.dir, child)) {
            char completed_day[9] = {};
            const bool completed_datalog_day =
                storage_export_datalog_day_from_path(frame.path,
                                                     completed_day,
                                                     sizeof(completed_day));
            {
                Storage::Guard guard;
                frame.dir.close();
                frame.opened = false;
            }
            walk_depth_--;
            budget--;
            if (completed_datalog_day) {
                out = StorageExportPlannerItem();
                out.kind = StorageExportPlannerItemKind::DatalogDayComplete;
                copy_cstr(out.datalog_day, sizeof(out.datalog_day),
                          completed_day);
                copy_cstr(out.path, sizeof(out.path), frame.path);
                return StorageExportPlannerResult::Item;
            }
            continue;
        }
        frame.next_index++;
        budget--;

        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(frame.path,
                                       child.name,
                                       child_path,
                                       sizeof(child_path)) ||
            !storage_user_path_valid(child_path)) {
            set_error(error_out, error_out_size, "bad_child_path");
            return StorageExportPlannerResult::Error;
        }

        if (strcmp(frame.path, "/DATALOG") == 0) {
            if (!child.is_dir ||
                !storage_export_datalog_day_allowed(child.name)) {
                continue;
            }
            if (config_.skip_completed_finalized_datalog_days &&
                datalog_day_finalized(child.name) &&
                datalog_day_done(child.name)) {
                continue;
            }
        }

        if (child.is_dir) {
            if (!push_dir(child_path, error_out, error_out_size)) {
                return StorageExportPlannerResult::Error;
            }
            continue;
        }

        if (build_file_item(child_path, false, out, error_out,
                            error_out_size)) {
            return StorageExportPlannerResult::Item;
        }
        if (error_out && error_out[0]) {
            return StorageExportPlannerResult::Error;
        }
    }
    return StorageExportPlannerResult::Yield;
}

bool StorageExportPlanner::reserve_datalog_days(size_t needed,
                                                char *error_out,
                                                size_t error_out_size) {
    if (needed <= datalog_day_capacity_) return true;
    size_t next = datalog_day_capacity_ == 0 ? 16 : datalog_day_capacity_ * 2;
    while (next < needed) next *= 2;
    DatalogDay *items = static_cast<DatalogDay *>(
        Memory::alloc_large(sizeof(DatalogDay) * next, false));
    if (!items) {
        set_error(error_out, error_out_size, "datalog_day_alloc");
        return false;
    }
    for (size_t i = 0; i < next; ++i) new (&items[i]) DatalogDay();
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        items[i] = datalog_days_[i];
    }
    if (datalog_days_) Memory::free(datalog_days_);
    datalog_days_ = items;
    datalog_day_capacity_ = next;
    return true;
}

bool StorageExportPlanner::add_datalog_day(const char *day,
                                           char *error_out,
                                           size_t error_out_size) {
    if (!day || !storage_export_is_datalog_day_name(day)) return true;
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        if (strcmp(datalog_days_[i].day, day) == 0) return true;
    }
    if (!reserve_datalog_days(datalog_day_count_ + 1,
                              error_out,
                              error_out_size)) {
        return false;
    }

    size_t pos = 0;
    while (pos < datalog_day_count_ &&
           strcmp(datalog_days_[pos].day, day) > 0) {
        pos++;
    }
    for (size_t i = datalog_day_count_; i > pos; --i) {
        datalog_days_[i] = datalog_days_[i - 1];
    }
    DatalogDay &entry = datalog_days_[pos];
    copy_cstr(entry.day, sizeof(entry.day), day);
    const int written = snprintf(entry.path, sizeof(entry.path),
                                 "/DATALOG/%s", day);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(entry.path)) {
        set_error(error_out, error_out_size, "datalog_day_path");
        return false;
    }
    datalog_day_count_++;
    return true;
}

StorageExportPlannerResult StorageExportPlanner::scan_datalog_days(
    char *error_out,
    size_t error_out_size,
    uint32_t &budget) {
    if (datalog_scan_complete_) return StorageExportPlannerResult::Done;
    if (!datalog_scan_opened_) {
        Storage::Guard guard;
        datalog_scan_dir_ = Storage::open("/DATALOG", "r");
        if (!datalog_scan_dir_ || !datalog_scan_dir_.isDirectory()) {
            if (datalog_scan_dir_) datalog_scan_dir_.close();
            datalog_scan_complete_ = true;
            return StorageExportPlannerResult::Done;
        }
        if (!storage_skip_dir_children(datalog_scan_dir_,
                                       datalog_scan_next_index_)) {
            datalog_scan_dir_.close();
            set_error(error_out, error_out_size, "datalog_scan_resume_failed");
            return StorageExportPlannerResult::Error;
        }
        datalog_scan_opened_ = true;
    }

    while (budget > 0) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(datalog_scan_dir_, child)) {
            {
                Storage::Guard guard;
                datalog_scan_dir_.close();
            }
            datalog_scan_opened_ = false;
            datalog_scan_complete_ = true;
            datalog_day_index_ = 0;
            return StorageExportPlannerResult::Done;
        }
        datalog_scan_next_index_++;
        budget--;
        if (!child.is_dir ||
            !storage_export_datalog_day_allowed(child.name)) {
            continue;
        }
        if (!add_datalog_day(child.name, error_out, error_out_size)) {
            return StorageExportPlannerResult::Error;
        }
    }
    return StorageExportPlannerResult::Yield;
}

bool StorageExportPlanner::select_next_datalog_day(char *error_out,
                                                   size_t error_out_size) {
    while (datalog_day_index_ < datalog_day_count_) {
        const DatalogDay &day = datalog_days_[datalog_day_index_++];
        if (config_.require_pending_datalog_file &&
            !datalog_day_has_pending_files(day, error_out, error_out_size)) {
            continue;
        }
        copy_cstr(day_name_, sizeof(day_name_), day.day);
        copy_cstr(day_path_, sizeof(day_path_), day.path);
        day_active_ = true;
        day_root_index_ = 0;
        return true;
    }
    return false;
}

StorageExportPlannerResult StorageExportPlanner::next_sleep_hq(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size,
    uint32_t &budget) {
    while (budget > 0) {
        if (!datalog_scan_complete_) {
            StorageExportPlannerResult scan =
                scan_datalog_days(error_out, error_out_size, budget);
            if (scan == StorageExportPlannerResult::Error ||
                scan == StorageExportPlannerResult::Yield) {
                return scan;
            }
        }

        if (!day_active_) {
            if (!select_next_datalog_day(error_out, error_out_size)) {
                return StorageExportPlannerResult::Done;
            }
        }

        const size_t root_count = sizeof(SLEEPHQ_ROOT_METADATA_FILES) /
                                  sizeof(SLEEPHQ_ROOT_METADATA_FILES[0]);
        while (day_root_index_ < root_count && budget > 0) {
            budget--;
            const char *path = SLEEPHQ_ROOT_METADATA_FILES[day_root_index_++];
            if (build_file_item(path, true, out, error_out, error_out_size)) {
                return StorageExportPlannerResult::Item;
            }
            if (error_out && error_out[0]) {
                return StorageExportPlannerResult::Error;
            }
        }
        if (budget == 0) return StorageExportPlannerResult::Yield;

        if (walk_depth_ == 0) {
            if (!push_dir(day_path_, error_out, error_out_size)) {
                return StorageExportPlannerResult::Error;
            }
        }
        StorageExportPlannerResult walked =
            next_walk_item(out, error_out, error_out_size, budget);
        if (walked == StorageExportPlannerResult::Item &&
            out.kind == StorageExportPlannerItemKind::DatalogDayComplete) {
            day_active_ = false;
            day_path_[0] = '\0';
            day_name_[0] = '\0';
            return walked;
        }
        if (walked != StorageExportPlannerResult::Yield) return walked;
    }
    return StorageExportPlannerResult::Yield;
}

}  // namespace aircannect
