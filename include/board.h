#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef AIRCANNECT_VERSION
#define AIRCANNECT_VERSION "dev"
#endif

#ifndef AIRCANNECT_BUILD_DATE
#define AIRCANNECT_BUILD_DATE __DATE__ " " __TIME__
#endif

#ifndef AC_STACK_PROFILE_ENABLED
#define AC_STACK_PROFILE_ENABLED 0
#endif

static constexpr uint32_t AC_STACK_PROFILE_SAMPLE_MS = 10000;
static constexpr uint32_t AC_STACK_PROFILE_SUMMARY_MS = 600000;

// Board configuration is split by subsystem; this keeps every
// #include "board.h" working unchanged.
#include "board_can.h"
#include "board_net.h"
#include "board_oximetry.h"
#include "board_report.h"
#include "board_storage.h"
