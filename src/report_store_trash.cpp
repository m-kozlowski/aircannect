#include "report_store.h"

#include <Arduino.h>
#include <new>

#include "memory_manager.h"
#include "report_store_internal.h"
#include "storage_directory.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStore {

using namespace ReportStoreInternal;

namespace {

struct TrashCleanupFrame {
    char path[REPORT_PATH_MAX] = {};
    bool opened = false;
    File dir;
};

struct TrashCleanupState {
    TrashCleanupFrame *frames = nullptr;
    size_t capacity = 0;
    size_t depth = 0;
};

TrashCleanupState trash_cleanup;

bool ensure_trash_cleanup_state() {
    if (trash_cleanup.frames) return true;

    trash_cleanup.frames = static_cast<TrashCleanupFrame *>(
        Memory::alloc_large(sizeof(TrashCleanupFrame) *
                            REPORT_TRASH_CLEANUP_MAX_DEPTH,
                            false));
    if (!trash_cleanup.frames) {
        note_error("trash_state_alloc_failed", &current.write_errors);
        return false;
    }

    trash_cleanup.capacity = REPORT_TRASH_CLEANUP_MAX_DEPTH;
    trash_cleanup.depth = 0;

    for (size_t i = 0; i < trash_cleanup.capacity; ++i) {
        new (&trash_cleanup.frames[i]) TrashCleanupFrame();
    }

    return true;
}

void close_trash_cleanup_walk() {
    if (!trash_cleanup.frames) return;

    for (size_t i = 0; i < trash_cleanup.depth; ++i) {
        if (trash_cleanup.frames[i].opened) {
            trash_cleanup.frames[i].dir.close();
            trash_cleanup.frames[i].opened = false;
        }
    }

    trash_cleanup.depth = 0;
}

bool push_trash_cleanup_dir(const char *path) {
    if (!path || !path[0] ||
        !ensure_trash_cleanup_state() ||
        trash_cleanup.depth >= trash_cleanup.capacity) {
        note_error("trash_cleanup_depth", &current.write_errors);
        return false;
    }

    TrashCleanupFrame &frame = trash_cleanup.frames[trash_cleanup.depth++];
    frame = TrashCleanupFrame();
    copy_cstr(frame.path, sizeof(frame.path), path);

    return true;
}

bool start_next_trash_cleanup_tree() {
    File root = Storage::open("/aircannect/report", "r");
    if (!root) return true;

    if (!root.isDirectory()) {
        root.close();
        return true;
    }

    bool ok = true;
    while (true) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(root, child)) break;
        if (!child.is_dir || !is_report_trash_dir_name(child.name)) continue;

        char child_path[REPORT_PATH_MAX];
        const bool path_ok = build_child_path("/aircannect/report",
                                              child.name,
                                              child_path,
                                              sizeof(child_path));
        if (!path_ok) {
            ok = false;
            note_error("trash_path_failed", &current.write_errors);
            break;
        }

        ok = push_trash_cleanup_dir(child_path);
        break;
    }

    root.close();
    return ok;
}

bool cleanup_trash_tree_step(uint32_t &budget, uint32_t &removed) {
    while (budget > 0 && trash_cleanup.depth > 0) {
        TrashCleanupFrame &frame =
            trash_cleanup.frames[trash_cleanup.depth - 1];
        if (!frame.opened) {
            frame.dir = Storage::open(frame.path, "r");
            if (!frame.dir) {
                if (!Storage::remove(frame.path)) return false;
                trash_cleanup.depth--;
                budget--;
                removed++;
                continue;
            }

            if (!frame.dir.isDirectory()) {
                frame.dir.close();
                if (!Storage::remove(frame.path)) return false;
                trash_cleanup.depth--;
                budget--;
                removed++;
                continue;
            }

            frame.opened = true;
        }

        StorageDirChild child;
        if (storage_read_next_dir_child(frame.dir, child)) {
            char child_path[REPORT_PATH_MAX];
            const bool path_ok = build_child_path(frame.path,
                                                  child.name,
                                                  child_path,
                                                  sizeof(child_path));
            if (!path_ok) {
                note_error("trash_child_path_failed", &current.write_errors);
                return false;
            }

            if (child.is_dir) {
                return push_trash_cleanup_dir(child_path);
            }

            if (!Storage::remove(child_path)) return false;
            budget--;
            removed++;
            continue;
        }

        frame.dir.close();
        frame.opened = false;

        char dir_path[REPORT_PATH_MAX];
        copy_cstr(dir_path, sizeof(dir_path), frame.path);

        trash_cleanup.depth--;
        if (!Storage::rmdir(dir_path)) return false;
        budget--;
        removed++;
    }

    return true;
}

}  // namespace

bool cleanup_trash_step(uint32_t max_entries, uint32_t &removed) {
    Storage::Guard g;

    removed = 0;
    if (!max_entries) return true;

    if (!ready()) {
        close_trash_cleanup_walk();
        return false;
    }

    if (!ensure_trash_cleanup_state()) return false;

    uint32_t budget = max_entries;
    bool ok = true;

    while (budget > 0) {
        if (trash_cleanup.depth == 0 &&
            !start_next_trash_cleanup_tree()) {
            ok = false;
            break;
        }

        if (trash_cleanup.depth == 0) break;

        if (!cleanup_trash_tree_step(budget, removed)) {
            ok = false;
            note_error("trash_cleanup_failed", &current.write_errors);
            break;
        }
    }

    if (!ok) {
        close_trash_cleanup_walk();
    }

    if (ok) set_error(current.last_error, sizeof(current.last_error), "");

    return ok;
}

}  // namespace ReportStore
}  // namespace aircannect
