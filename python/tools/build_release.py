#!/usr/bin/env python3
"""Build and package firmware release artifacts."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import zlib


PROJECT_DIR = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_ENVIRONMENTS = (
    "xiao-esp32s3-plus-sdmmc4",
    "xiao-esp32s3-plus-spisd",
)
READ_CHUNK_BYTES = 256 * 1024


def git_output(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=PROJECT_DIR, text=True
    ).strip()


def source_identity() -> tuple[str, str, int]:
    version = git_output("describe", "--tags", "--always", "--dirty")
    commit = git_output("rev-parse", "HEAD")
    commit_epoch = int(git_output("show", "-s", "--format=%ct", "HEAD"))
    return version, commit, commit_epoch


def filename_component(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-.")
    return sanitized or "unknown"


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(READ_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def compress_zlib(source_path: pathlib.Path,
                  destination_path: pathlib.Path) -> None:
    compressor = zlib.compressobj(level=6)
    with source_path.open("rb") as source, destination_path.open("wb") as destination:
        while chunk := source.read(READ_CHUNK_BYTES):
            destination.write(compressor.compress(chunk))
        destination.write(compressor.flush())


def validate_firmware(path: pathlib.Path) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"firmware image is missing or empty: {path}")
    with path.open("rb") as source:
        if source.read(1) != b"\xe9":
            raise RuntimeError(f"not an esp32 application image: {path}")


def build_firmware(pio: str, environment_name: str,
                   source_date_epoch: int, version: str) -> pathlib.Path:
    build_env = os.environ.copy()
    build_env["AIRCANNECT_VERSION"] = version
    build_env["SOURCE_DATE_EPOCH"] = str(source_date_epoch)
    subprocess.run(
        [pio, "run", "-e", environment_name],
        cwd=PROJECT_DIR,
        env=build_env,
        check=True,
    )

    firmware_path = (
        PROJECT_DIR / ".pio" / "build" / environment_name / "firmware.bin"
    )
    validate_firmware(firmware_path)
    return firmware_path


def artifact_record(path: pathlib.Path, kind: str) -> dict[str, object]:
    return {
        "file": path.name,
        "kind": kind,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def package_firmware(
    firmware_path: pathlib.Path, output_dir: pathlib.Path,
    environment_name: str, version: str,
) -> list[dict[str, object]]:
    stem = "-".join((
        "aircannect",
        filename_component(version),
        filename_component(environment_name),
    ))
    raw_path = output_dir / f"{stem}.bin"
    compressed_path = output_dir / f"{stem}.bin.zlib"
    shutil.copyfile(firmware_path, raw_path)
    compress_zlib(raw_path, compressed_path)

    return [
        artifact_record(raw_path, "esp32-application"),
        artifact_record(compressed_path, "esp32-application-zlib"),
    ]


def write_checksums(output_dir: pathlib.Path, artifacts: list[dict[str, object]]) -> None:
    checksum_lines = [
        f"{artifact['sha256']}  {artifact['file']}" for artifact in artifacts
    ]
    (output_dir / "SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="utf-8"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--env",
        action="append",
        dest="environments",
        metavar="NAME",
        help="PlatformIO environment to build; repeat to select multiple targets",
    )
    parser.add_argument("--pio", default="pio",
                        help="PlatformIO executable")
    parser.add_argument("--output", type=pathlib.Path,
                        default=PROJECT_DIR / "dist",
                        help="artifact output directory")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_dir = args.output.resolve()
    version, commit, source_date_epoch = source_identity()
    environments = args.environments or DEFAULT_ENVIRONMENTS

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    artifacts = []
    for environment_name in environments:
        firmware_path = build_firmware(
            args.pio, environment_name, source_date_epoch, version
        )
        artifacts.extend(package_firmware(
            firmware_path, output_dir, environment_name, version
        ))

    write_checksums(output_dir, artifacts)

    print(
        f"packaged {version} ({commit[:12]}) for "
        f"{', '.join(environments)} in {output_dir}"
    )


if __name__ == "__main__":
    main()
