#!/usr/bin/env python3
"""Build release notes from commits since the previous release tag."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


PROJECT_DIR = pathlib.Path(__file__).resolve().parents[2]
VERSION_TAG_RE = re.compile(
    r"^v(?P<major>[0-9]+)\.(?P<minor>[0-9]+)\.(?P<patch>[0-9]+)"
    r"(?P<suffix>[-+][0-9A-Za-z][0-9A-Za-z.-]*)?$"
)
IGNORED_SUBJECTS = frozenset({"sync"})
IGNORED_SUBJECT_PREFIXES = (
    "tests:",
    "agents:",
    "ci:",
    "[tests]",
    "[agents]",
    "[ci]",
)
SUBJECT_GROUP_RE = re.compile(
    r"^(?P<prefix>[A-Za-z0-9][A-Za-z0-9_. -]*):\s+(?P<summary>.+)$"
)
GROUP_LABELS = {
    "edf": "EDF",
    "shq": "SleepHQ",
    "smb": "SMB",
    "exports": "Exports",
    "reports": "Reports",
    "oximetry": "Oximetry",
    "webui": "Web UI",
    "storage": "Storage",
    "ota": "OTA",
    "wifi": "Wi-Fi",
    "can": "CAN",
    "rpc": "RPC",
    "ble": "BLE",
    "worker": "Worker",
    "cli": "CLI",
    "logging": "Logging",
    "runtime": "Runtime",
    "maintenance": "Maintenance",
    "build": "Build",
    "documentation": "Documentation",
}
GROUP_ALIASES = {
    "parsing": "edf",
    "edf capture": "edf",
    "report": "reports",
    "report ui": "reports",
    "report prefetch": "reports",
    "oxi": "oximetry",
    "web": "webui",
    "web ui": "webui",
    "file log": "logging",
    "log": "logging",
    "loging": "logging",
    "resmed ota": "ota",
    "pio": "build",
    "docs": "documentation",
}
GROUP_PRIORITIES = {
    "edf": 0,
    "shq": 10,
    "smb": 10,
    "exports": 10,
    "reports": 20,
    "oximetry": 30,
    "webui": 40,
    "storage": 50,
    "ota": 60,
    "wifi": 70,
    "can": 80,
    "rpc": 80,
    "ble": 90,
    "worker": 100,
    "cli": 110,
    "logging": 120,
    "runtime": 130,
    "maintenance": 140,
    "build": 150,
    "documentation": 2000,
}
UNCLASSIFIED_PRIORITY = 1000


class ReleaseNotesError(RuntimeError):
    """A release tag or its Git history cannot produce useful notes."""


def git_output(repository: pathlib.Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repository,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ReleaseNotesError(
            f"git {' '.join(args)} failed: {detail or 'unknown error'}"
        )
    return result.stdout.strip()


def git_revision_exists(repository: pathlib.Path, revision: str) -> bool:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", revision],
        cwd=repository,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def validate_release_tag(repository: pathlib.Path, tag: str) -> None:
    if not VERSION_TAG_RE.fullmatch(tag):
        raise ReleaseNotesError(
            f"release tag does not match vN.N.N[-suffix]: {tag}"
        )

    if not git_revision_exists(repository, f"refs/tags/{tag}^{{commit}}"):
        raise ReleaseNotesError(f"release tag does not exist: {tag}")


def release_version_key(tag: str) -> tuple[object, ...]:
    match = VERSION_TAG_RE.fullmatch(tag)
    if match is None:
        raise ReleaseNotesError(f"invalid release tag: {tag}")

    suffix = match.group("suffix")
    suffix_parts = ()
    if suffix:
        suffix_parts = tuple(
            (0, int(part)) if part.isdigit() else (1, part.casefold())
            for part in re.findall(r"[0-9]+|[A-Za-z]+", suffix[1:])
        )

    return (
        int(match.group("major")),
        int(match.group("minor")),
        int(match.group("patch")),
        0 if suffix else 1,
        suffix_parts,
    )


def previous_release_tag(repository: pathlib.Path, tag: str) -> str | None:
    parent_revision = f"refs/tags/{tag}^{{commit}}^"
    if not git_revision_exists(repository, parent_revision):
        return None

    reachable = git_output(
        repository, "tag", "--merged", parent_revision
    ).splitlines()
    current_version = release_version_key(tag)
    candidates = [
        candidate
        for candidate in reachable
        if (
            candidate != tag
            and VERSION_TAG_RE.fullmatch(candidate)
            and release_version_key(candidate) < current_version
        )
    ]
    if not candidates:
        return None

    return max(candidates, key=lambda candidate: (
        release_version_key(candidate), candidate
    ))


def release_subjects(
    repository: pathlib.Path, tag: str, previous_tag: str | None
) -> list[str]:
    revision_range = f"{previous_tag}..{tag}" if previous_tag else tag
    subjects = git_output(
        repository,
        "log",
        "--no-merges",
        "--reverse",
        "--format=%s",
        revision_range,
    ).splitlines()

    included = []
    for subject in subjects:
        subject = subject.strip()
        normalized = subject.casefold()
        if not subject or normalized in IGNORED_SUBJECTS:
            continue
        if normalized.startswith(IGNORED_SUBJECT_PREFIXES):
            continue
        included.append(subject)
    return included


def manual_notes(
    repository: pathlib.Path, notes_directory: pathlib.Path, tag: str
) -> str:
    directory = (
        notes_directory
        if notes_directory.is_absolute()
        else repository / notes_directory
    )
    path = directory / f"{tag}.md"
    if not path.exists():
        return ""
    if not path.is_file():
        raise ReleaseNotesError(f"manual release notes are not a file: {path}")
    return path.read_text(encoding="utf-8").strip()


def normalize_group_name(value: str) -> str:
    value = value.casefold().replace("_", " ").replace("-", " ")
    return " ".join(value.split())


def classify_group(prefix: str) -> tuple[str, int]:
    parts = [normalize_group_name(part) for part in prefix.split(".")]
    combined = " ".join(parts)
    if combined in GROUP_ALIASES:
        canonical = GROUP_ALIASES[combined]
        return GROUP_LABELS[canonical], GROUP_PRIORITIES[canonical]

    labels = []
    priority = UNCLASSIFIED_PRIORITY
    for index, part in enumerate(parts):
        canonical = GROUP_ALIASES.get(part, part)
        labels.append(GROUP_LABELS.get(canonical, part.title()))
        if index == 0:
            priority = GROUP_PRIORITIES.get(
                canonical, UNCLASSIFIED_PRIORITY
            )
    return " / ".join(labels), priority


def group_subjects(subjects: list[str]) -> list[tuple[str, list[str]]]:
    groups: dict[str, list[str]] = {}
    sort_keys: dict[str, tuple[int, int]] = {}
    for subject_index, subject in enumerate(subjects):
        match = SUBJECT_GROUP_RE.fullmatch(subject)
        if match is None:
            label = "Other changes"
            priority = UNCLASSIFIED_PRIORITY
            summary = subject
        else:
            label, priority = classify_group(match.group("prefix"))
            summary = match.group("summary")

        if label not in groups:
            groups[label] = []
            sort_keys[label] = (priority, subject_index)
        groups[label].append(summary)

    ordered = sorted(
        groups.items(),
        key=lambda item: sort_keys[item[0]],
    )
    return ordered


def render_release_notes(
    previous_tag: str | None, subjects: list[str], preface: str = ""
) -> str:
    if not subjects:
        if preface:
            return preface + "\n"
        raise ReleaseNotesError(
            "no release changes remain after filtering repository-only commits"
        )

    heading = (
        f"## Changes since {previous_tag}"
        if previous_tag
        else "## Initial release"
    )
    generated_lines = [heading]
    for label, entries in group_subjects(subjects):
        generated_lines.extend(("", f"### {label}", ""))
        generated_lines.extend(f"- {entry}" for entry in entries)

    generated = "\n".join(generated_lines)
    sections = [section for section in (preface, generated) if section]
    return "\n\n".join(sections) + "\n"


def build_release_notes(
    repository: pathlib.Path, tag: str, notes_directory: pathlib.Path
) -> tuple[str, str | None]:
    repository = repository.resolve()
    validate_release_tag(repository, tag)

    previous_tag = previous_release_tag(repository, tag)
    subjects = release_subjects(repository, tag, previous_tag)
    preface = manual_notes(repository, notes_directory, tag)
    return render_release_notes(previous_tag, subjects, preface), previous_tag


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="release tag to describe")
    parser.add_argument(
        "--output",
        required=True,
        type=pathlib.Path,
        help="Markdown file to write",
    )
    parser.add_argument(
        "--notes-dir",
        type=pathlib.Path,
        default=pathlib.Path("docs/releases"),
        help="optional directory containing <tag>.md release prefaces",
    )
    parser.add_argument(
        "--repository",
        type=pathlib.Path,
        default=PROJECT_DIR,
        help=argparse.SUPPRESS,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        notes, previous_tag = build_release_notes(
            args.repository, args.tag, args.notes_dir
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(notes, encoding="utf-8")
    except (OSError, ReleaseNotesError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    baseline = previous_tag or "initial release"
    print(f"release notes for {args.tag} use baseline {baseline}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
