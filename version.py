#!/usr/bin/env python3
import datetime
import os
import subprocess


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


print(
    shell_define("AIRCANNECT_VERSION", git_version()),
    shell_define("AIRCANNECT_BUILD_DATE", build_date()),
)
