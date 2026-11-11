#!/usr/bin/env python3
"""Build patched TCP/SSE sources without changing the installed libraries."""

from pathlib import Path

from library_patches import build_patched_source


def build_source(env, node):
    original = Path(node.srcnode().get_abspath())
    patches = {
        "AsyncTCP.cpp": "asynctcp",
        "AsyncEventSource.cpp": "espasyncwebserver-sse",
    }[original.name]
    return build_patched_source(env, node, patches)


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/AsyncTCP/src/AsyncTCP.cpp")
    env.AddBuildMiddleware(build_source, "*/ESPAsyncWebServer/src/AsyncEventSource.cpp")
