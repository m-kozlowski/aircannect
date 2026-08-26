#pragma once

#include <stddef.h>

#include "local_input.h"

#define AC_BUTTON_PROFILE_NONE 0
#define AC_BUTTON_PROFILE_WAVESHARE_154 1

#ifndef AC_BUTTON_PROFILE
#define AC_BUTTON_PROFILE AC_BUTTON_PROFILE_NONE
#endif

namespace aircannect {

const BoardButtonDefinition *board_button_catalog(size_t &count);
const BoardButtonDefinition *board_button_find(uint16_t key);
const BoardButtonDefinition *board_button_find(const char *id);

}  // namespace aircannect
