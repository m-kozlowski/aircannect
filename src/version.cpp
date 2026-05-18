#include "version.h"

#ifndef AIRCANNECT_VERSION
#define AIRCANNECT_VERSION "unknown"
#endif

#ifndef AIRCANNECT_BUILD_DATE
#define AIRCANNECT_BUILD_DATE "unknown"
#endif

const char *aircannect_version() {
    return AIRCANNECT_VERSION;
}

const char *aircannect_build_date() {
    return AIRCANNECT_BUILD_DATE;
}
