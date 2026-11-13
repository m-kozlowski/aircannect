"""Bound the shared object cache; keep dependency state local to each build."""

import atexit
import os
import re
import sys

Import("env")

CACHE_LIMIT_BYTES = 5_000_000_000


def trim_cache(cache):
    entries = []
    try:
        with os.scandir(cache.path) as buckets:
            for bucket in buckets:
                if not bucket.is_dir(follow_symlinks=False):
                    continue
                if not re.fullmatch(r"[0-9A-F]+", bucket.name):
                    continue

                with os.scandir(bucket.path) as files:
                    for entry in files:
                        # Do not touch SCons metadata or in-progress copies.
                        if not re.fullmatch(r"[0-9a-f]{32,64}", entry.name):
                            continue
                        if not entry.is_file(follow_symlinks=False):
                            continue
                        try:
                            info = entry.stat(follow_symlinks=False)
                        except FileNotFoundError:
                            continue
                        entries.append((info.st_mtime_ns, entry.path, info.st_size))

        total = sum(size for _, _, size in entries)
        if total <= CACHE_LIMIT_BYTES:
            return

        removed = 0
        # SCons refreshes mtime whenever an entry is retrieved.
        for modified, path, size in sorted(entries):
            if total <= CACHE_LIMIT_BYTES:
                break
            try:
                if os.stat(path, follow_symlinks=False).st_mtime_ns != modified:
                    continue
                os.unlink(path)
                removed += 1
            except FileNotFoundError:
                pass
            total -= size

        if removed:
            print(f"Build cache: removed {removed} old entries (limit 5 GB)")
    except OSError as error:
        print(f"Warning: could not trim build cache: {error}", file=sys.stderr)


if not env.get("_AC_CACHE_LIMIT_REGISTERED") and not env.GetOption("no_exec"):
    cache = env.get_CacheDir()
    if cache.is_enabled() and not cache.is_readonly():
        atexit.register(trim_cache, cache)
        env["_AC_CACHE_LIMIT_REGISTERED"] = True

# Run before dependency scanning, and again after platform target naming.
# SConsign stores relative paths and cannot describe two checkouts at once.
env.SConsignFile(os.path.join(
    env.subst("$BUILD_DIR"),
    ".sconsign%d%d" % (sys.version_info[0], sys.version_info[1]),
))

env.NoCache([
    env.subst("$PROGPATH"),
    env.subst("$BUILD_DIR/${PROGNAME}.bin"),
    env.subst("$BUILD_DIR/${PROGNAME}.factory.bin"),
])
