"""Apply SPI SD fixes and diagnostics without changing framework sources."""

from library_patches import build_patched_source


def build_source(env, node):
    env.AppendUnique(CPPPATH=[env["PROJECT_INCLUDE_DIR"]])
    return build_patched_source(env, node, "sd-spi")


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/libraries/SD/src/sd_diskio.cpp")
