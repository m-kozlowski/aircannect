#!/usr/bin/env python3
"""Build the patched AsyncTCP source without changing the installed library."""

from library_patches import build_patched_source


def build_source(env, node):
    return build_patched_source(env, node, "asynctcp")


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/AsyncTCP/src/AsyncTCP.cpp")
