from SCons.Script import DefaultEnvironment

import hashlib
import os
from pathlib import Path
import shutil
import subprocess


env = DefaultEnvironment()
PROJECT_DIR = Path(env["PROJECT_DIR"]).resolve()

UPSTREAM_DIR = PROJECT_DIR / "third_party" / "libsmb2"
OVERLAY_DIR = PROJECT_DIR / "third_party" / "libsmb2-pio"
PATCH_DIR = PROJECT_DIR / "patches" / "libsmb2"
GENERATED_DIR = PROJECT_DIR / ".pio" / "generated-libs" / "libsmb2"
STAMP_PATH = GENERATED_DIR / ".aircannect-stamp"

SCRIPT_VERSION = "2"


def fail(message):
    raise RuntimeError(f"libsmb2 generation failed: {message}")


def run(cmd, cwd=None):
    return subprocess.check_output(cmd, cwd=cwd, text=True).strip()


def run_checked(cmd, cwd=None):
    subprocess.check_call(cmd, cwd=cwd)


def hash_file(path, digest):
    digest.update(str(path.relative_to(PROJECT_DIR)).encode("utf-8"))
    digest.update(b"\0")
    digest.update(path.read_bytes())
    digest.update(b"\0")


def hash_tree(path):
    digest = hashlib.sha256()
    if not path.exists():
        return digest.hexdigest()
    for item in sorted(p for p in path.rglob("*") if p.is_file()):
        hash_file(item, digest)
    return digest.hexdigest()


def patch_files():
    if not PATCH_DIR.exists():
        return []
    return sorted(PATCH_DIR.glob("*.patch"))


def upstream_commit():
    if not (UPSTREAM_DIR / ".git").exists():
        try:
            run_checked([
                "git",
                "submodule",
                "update",
                "--init",
                "--",
                "third_party/libsmb2",
            ], cwd=PROJECT_DIR)
        except subprocess.CalledProcessError as exc:
            fail("third_party/libsmb2 auto-init failed; "
                 "run git submodule update --init "
                 f"(exit {exc.returncode})")
    if not (UPSTREAM_DIR / ".git").exists():
        fail("third_party/libsmb2 is not initialized")
    return run(["git", "rev-parse", "HEAD"], cwd=UPSTREAM_DIR)


def ensure_upstream_clean():
    status = run(["git", "status", "--porcelain"], cwd=UPSTREAM_DIR)
    if status:
        fail("third_party/libsmb2 has local changes")


def desired_stamp():
    digest = hashlib.sha256()
    digest.update(SCRIPT_VERSION.encode("utf-8"))
    digest.update(upstream_commit().encode("utf-8"))
    digest.update(hash_tree(OVERLAY_DIR).encode("utf-8"))
    for patch in patch_files():
        hash_file(patch, digest)
    return digest.hexdigest()


def copy_dir(src, dst):
    if not src.exists():
        fail(f"missing required path {src}")
    shutil.copytree(src, dst, dirs_exist_ok=True)


def materialize():
    ensure_upstream_clean()
    stamp = desired_stamp()
    if STAMP_PATH.exists() and STAMP_PATH.read_text().strip() == stamp:
        return

    if GENERATED_DIR.exists():
        shutil.rmtree(GENERATED_DIR)
    GENERATED_DIR.mkdir(parents=True)

    for name in ("include", "lib"):
        copy_dir(UPSTREAM_DIR / name, GENERATED_DIR / name)
    for name in ("COPYING", "LICENCE-LGPL-2.1.txt", "README"):
        src = UPSTREAM_DIR / name
        if src.exists():
            shutil.copy2(src, GENERATED_DIR / name)

    copy_dir(OVERLAY_DIR, GENERATED_DIR)

    for patch in patch_files():
        subprocess.check_call(["git", "apply", str(patch)], cwd=GENERATED_DIR)

    STAMP_PATH.write_text(stamp + "\n")


materialize()
