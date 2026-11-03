#pragma once

#include <stddef.h>

namespace aircannect {

static constexpr size_t AC_STORAGE_PATH_MAX = 192;
static constexpr size_t AC_STORAGE_NAME_MAX = 80;
static constexpr size_t AC_STORAGE_ERROR_MAX = 64;
static constexpr size_t AC_STORAGE_MAX_SELECTIONS = 64;
static constexpr const char *AC_STORAGE_RETAINED_LINKS_ROOT =
    "/aircannect/report/layout-links";

const char *storage_basename_from_path(const char *path);
bool storage_append_child_path(const char *parent,
                               const char *name,
                               char *out,
                               size_t out_size);
bool storage_valid_child_name(const char *name);
bool storage_user_path_valid(const char *path);
// Lexical match on valid user paths; the storage owner checks existence.
bool storage_path_retained_links(const char *path, bool include_ancestors = false);
bool storage_resolve_user_path(const char *base,
                               const char *path,
                               char *out,
                               size_t out_size);
void storage_normalize_path(char *path);

}  // namespace aircannect
