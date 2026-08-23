from SCons.Script import DefaultEnvironment

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


env = DefaultEnvironment()
PROJECT_DIR = Path(env["PROJECT_DIR"]).resolve()

UPSTREAM_DIR = PROJECT_DIR / "third_party" / "libsmb2"
OVERLAY_DIR = PROJECT_DIR / "third_party" / "libsmb2-pio"
PATCH_DIR = PROJECT_DIR / "patches" / "libsmb2"
LIBSMB2_COMMIT = "b1b3887993e0cee5242d7fdebc116a0578bf29b8"
FETCHED_UPSTREAM_DIR = PROJECT_DIR / ".pio" / "downloaded-libs" / (
    f"libsmb2-{LIBSMB2_COMMIT}")
GENERATED_DIR = PROJECT_DIR / ".pio" / "generated-libs" / "libsmb2"
STAMP_PATH = GENERATED_DIR / ".aircannect-stamp"

SCRIPT_VERSION = "4"


def fail(message):
    raise RuntimeError(f"libsmb2 generation failed: {message}")


def run(cmd, cwd=None):
    return subprocess.check_output(cmd, cwd=cwd, text=True).strip()


def run_completed(cmd, cwd=None):
    return subprocess.run(cmd,
                          cwd=cwd,
                          text=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def git_repo_available(path):
    if not path.exists():
        return False
    result = run_completed(["git", "rev-parse", "--is-inside-work-tree"],
                           cwd=path)
    return result.returncode == 0 and result.stdout.strip() == "true"


def source_available(path):
    return (path / "include").is_dir() and (path / "lib").is_dir()


def upstream_source_available():
    return source_available(UPSTREAM_DIR)


def fetched_source_available():
    return source_available(FETCHED_UPSTREAM_DIR)


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


def safe_extract_tar(tar, dst):
    dst_resolved = dst.resolve()
    for member in tar.getmembers():
        target = (dst / member.name).resolve()
        if not (target == dst_resolved or
                str(target).startswith(str(dst_resolved) + os.sep)):
            fail("libsmb2 archive contains unsafe path")
    tar.extractall(dst)


def fetch_upstream_archive():
    if fetched_source_available():
        return
    url = (
        "https://github.com/sahlberg/libsmb2/archive/"
        f"{LIBSMB2_COMMIT}.tar.gz")
    FETCHED_UPSTREAM_DIR.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp_text:
        tmp = Path(tmp_text)
        archive = tmp / "libsmb2.tar.gz"
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                archive.write_bytes(response.read())
        except Exception as exc:
            fail("third_party/libsmb2 source is missing and upstream download "
                 f"failed: {exc}")
        with tarfile.open(archive, "r:gz") as tar:
            safe_extract_tar(tar, tmp)
        roots = [p for p in tmp.iterdir()
                 if p.is_dir() and source_available(p)]
        if len(roots) != 1:
            fail("libsmb2 archive did not contain expected source tree")
        if FETCHED_UPSTREAM_DIR.exists():
            shutil.rmtree(FETCHED_UPSTREAM_DIR)
        shutil.copytree(roots[0], FETCHED_UPSTREAM_DIR)


def upstream_source_dir():
    if git_repo_available(UPSTREAM_DIR) or upstream_source_available():
        return UPSTREAM_DIR
    if not fetched_source_available():
        fetch_upstream_archive()
    return FETCHED_UPSTREAM_DIR


def upstream_commit():
    if not git_repo_available(UPSTREAM_DIR):
        narrow = run_completed([
            "git",
            "submodule",
            "update",
            "--init",
            "--",
            "third_party/libsmb2",
        ], cwd=PROJECT_DIR)
        if narrow.returncode != 0:
            broad = run_completed([
                "git",
                "submodule",
                "update",
                "--init",
                "--recursive",
            ], cwd=PROJECT_DIR)
            if broad.returncode != 0:
                detail = (broad.stderr or narrow.stderr).strip()
                if detail:
                    detail = f": {detail}"
                fail("third_party/libsmb2 auto-init failed; "
                     "run git submodule update --init --recursive"
                     f"{detail}")
        if not git_repo_available(UPSTREAM_DIR):
            fail("third_party/libsmb2 auto-init failed; "
                 "run git submodule update --init --recursive")
    if not git_repo_available(UPSTREAM_DIR):
        fail("third_party/libsmb2 is not initialized")
    return run(["git", "rev-parse", "HEAD"], cwd=UPSTREAM_DIR)


def ensure_upstream_clean():
    if not git_repo_available(UPSTREAM_DIR):
        if upstream_source_available():
            return
        if git_repo_available(PROJECT_DIR):
            upstream_commit()
        else:
            fetch_upstream_archive()
            return
    if not git_repo_available(UPSTREAM_DIR):
        fail("third_party/libsmb2 source is missing; use a recursive git clone "
             "or a release archive that includes third_party/libsmb2")
    status = run(["git", "status", "--porcelain"], cwd=UPSTREAM_DIR)
    if status:
        fail("third_party/libsmb2 has local changes")


def upstream_identity():
    if git_repo_available(UPSTREAM_DIR):
        return f"git:{upstream_commit()}"
    if upstream_source_available():
        return f"tree:{hash_tree(UPSTREAM_DIR)}"
    fetch_upstream_archive()
    return f"archive:{LIBSMB2_COMMIT}:{hash_tree(FETCHED_UPSTREAM_DIR)}"


def desired_stamp():
    digest = hashlib.sha256()
    digest.update(SCRIPT_VERSION.encode("utf-8"))
    digest.update(upstream_identity().encode("utf-8"))
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

    source_dir = upstream_source_dir()
    for name in ("include", "lib"):
        copy_dir(source_dir / name, GENERATED_DIR / name)
    for name in ("COPYING", "LICENCE-LGPL-2.1.txt", "README"):
        src = source_dir / name
        if src.exists():
            shutil.copy2(src, GENERATED_DIR / name)

    copy_dir(OVERLAY_DIR, GENERATED_DIR)

    for patch in patch_files():
        subprocess.check_call(["git", "apply", str(patch)], cwd=GENERATED_DIR)

    STAMP_PATH.write_text(stamp + "\n")


materialize()
