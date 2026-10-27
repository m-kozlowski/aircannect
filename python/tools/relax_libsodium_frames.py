#!/usr/bin/env python3
"""Build the patched libsodium source while retaining frame-size checks.

The ref10 ge25519_scalarmult implementation has one oversized stack frame.
Keep its diagnostic visible as a warning; other functions retain the project's
-Werror=frame-larger-than check. The firmware link discards this unused function.
"""

from pathlib import Path

from library_patches import build_patched_source


def build_source(env, node):
    source_dir = Path(node.srcnode().get_abspath()).parent
    env.AppendUnique(CPPPATH=[str(source_dir)])
    return build_patched_source(env, node, "libsodium")


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(
        build_source,
        "*/libsodium/libsodium/src/libsodium/crypto_core/ed25519/ref10/ed25519_ref10.c",
    )
