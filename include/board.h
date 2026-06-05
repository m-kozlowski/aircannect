#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef AIRCANNECT_VERSION
#define AIRCANNECT_VERSION "dev"
#endif

#ifndef AIRCANNECT_BUILD_DATE
#define AIRCANNECT_BUILD_DATE __DATE__ " " __TIME__
#endif

// Board configuration is split by subsystem; this keeps every
// #include "board.h" working unchanged.
#include "board_can.h"
#include "board_net.h"
#include "board_oximetry.h"
#include "board_report.h"
#include "board_storage.h"
