#pragma once

#include <stddef.h>

namespace aircannect {

static constexpr size_t AC_STORAGE_PATH_MAX = 192;
static constexpr size_t AC_STORAGE_NAME_MAX = 80;
static constexpr size_t AC_STORAGE_ERROR_MAX = 64;
static constexpr size_t AC_STORAGE_MAX_SELECTIONS = 64;

const char *storage_basename_from_path(const char *path);
bool storage_append_child_path(const char *parent,
                               const char *name,
                               char *out,
                               size_t out_size);
bool storage_valid_child_name(const char *name);
bool storage_user_path_valid(const char *path);
void storage_normalize_path(char *path);

}  // namespace aircannect
