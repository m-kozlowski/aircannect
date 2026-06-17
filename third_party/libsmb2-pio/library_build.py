Import("env")
import os

lib_source_dir = os.path.join(
    env.subst("$PROJECT_DIR"), ".pio", "generated-libs", "libsmb2"
)

env.Append(CPPPATH=[
    os.path.join(lib_source_dir, "include"),
    os.path.join(lib_source_dir, "include/smb2"),
    os.path.join(lib_source_dir, "include/esp"),
    os.path.join(lib_source_dir, "lib"),
])

env.Append(CPPDEFINES=[
    "HAVE_CONFIG_H",
    "NEED_READV",
    "NEED_WRITEV",
    "NEED_GETLOGIN_R",
    "NEED_RANDOM",
    "NEED_SRANDOM",
    ("_U_", ""),
])

env.Append(CCFLAGS=[
    "-Wno-implicit-function-declaration",
    "-Wno-builtin-declaration-mismatch",
    "-include", os.path.join(lib_source_dir, "lib/esp_compat_wrapper.h"),
])
