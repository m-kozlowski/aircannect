"""Build the patched esp_wireguard platform source without editing its install."""

from library_patches import build_patched_source


def build_source(env, node):
    return build_patched_source(env, node, "wireguard-platform")


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/esp_wireguard/src/wireguard-platform.c")
