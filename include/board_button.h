#pragma once

#include <stddef.h>

#include <stdint.h>

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
bool board_button_input_find(const char *id, ButtonInput &input);
bool board_button_input_supported(const ButtonInput &input,
                                  ButtonGesture gesture);
bool board_button_input_id(const ButtonInput &input,
                           char *out,
                           size_t out_size);
bool board_button_input_label(const ButtonInput &input,
                              char *out,
                              size_t out_size);

}  // namespace aircannect
