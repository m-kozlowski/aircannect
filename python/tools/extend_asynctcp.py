#!/usr/bin/env python3
"""Build the patched AsyncTCP source without changing the installed library."""

from pathlib import Path
import shutil
import tempfile

from library_patches import apply_patches, patch_files


def build_source(env, node):
    original = Path(node.srcnode().get_abspath())
    output = Path(env.subst("$BUILD_DIR")) / "generated" / "AsyncTCP.cpp"
    patches = patch_files(Path(env["PROJECT_DIR"]) / "patches" / "asynctcp")
    if not patches:
        raise RuntimeError("AsyncTCP patches are missing")
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        staging = Path(temporary)
        source = staging / original.name
        shutil.copyfile(original, source)
        apply_patches(staging, patches)
        content = source.read_bytes()

    if not output.exists() or output.read_bytes() != content:
        output.write_bytes(content)
    return env.File(str(output))


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/AsyncTCP/src/AsyncTCP.cpp")
