#!/usr/bin/env python3
import datetime
import os
import sys
import subprocess
from pathlib import Path


def shell_define(name, value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'-D{name}=\\"{escaped}\\"'


def git_version():
    override = os.environ.get("AIRCANNECT_VERSION")
    if override:
        return override
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return "unknown"


def build_date():
    override = os.environ.get("AIRCANNECT_BUILD_DATE")
    if override:
        return override
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            dt = datetime.datetime.fromtimestamp(
                int(epoch), datetime.timezone.utc
            )
            return dt.strftime("%Y-%m-%dT%H:%M")
        except ValueError:
            pass
    return datetime.datetime.now().strftime("%Y-%m-%dT%H:%M")


def c_string(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')


def write_header(path):
    version = c_string(git_version())
    date = c_string(build_date())
    content = (
        "// Auto-generated from version.py, do not edit.\n"
        "#pragma once\n\n"
        f'#define AIRCANNECT_VERSION "{version}"\n'
        f'#define AIRCANNECT_BUILD_DATE "{date}"\n'
    )
    target = Path(path)
    if target.exists() and target.read_text(encoding="utf-8") == content:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--header":
        write_header(sys.argv[2])
    else:
        print(
            shell_define("AIRCANNECT_VERSION", git_version()),
            shell_define("AIRCANNECT_BUILD_DATE", build_date()),
        )
