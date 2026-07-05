#!/usr/bin/env python3
"""Flash AirCANnect firmware through the device HTTP OTA endpoint.

This deliberately uses the same HTTP API as the web UI instead of PlatformIO
espota. That makes it usable from WSL/NAT environments where espota callbacks
are unreliable.
"""

from __future__ import annotations

import argparse
import base64
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import time
import uuid
import zlib
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlencode, urlparse


DEFAULT_ENV = "xiao-esp32s3-plus-sdmmc4"
DEFAULT_HOST = "aircannect"
DEFAULT_USER = "admin"
DEFAULT_PASSWORD = "aircannect"


@dataclass(frozen=True)
class Target:
    scheme: str
    host: str
    port: int
    base_path: str


@dataclass(frozen=True)
class UploadPayload:
    filename: str
    data: bytes
    raw_size: int
    wire_size: int
    encoding: str


def die(message: str, code: int = 1) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(code)


def format_bytes(value: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(value)
    for unit in units:
        if amount < 1024 or unit == units[-1]:
            return f"{amount:.1f} {unit}" if unit != "B" else f"{value} B"
        amount /= 1024
    return f"{value} B"


def parse_target(text: str) -> Target:
    if "://" not in text:
        text = "http://" + text
    parsed = urlparse(text)
    if parsed.scheme != "http":
        die("only http:// targets are supported")
    if not parsed.hostname:
        die("missing target host")
    return Target(
        scheme=parsed.scheme,
        host=parsed.hostname,
        port=parsed.port or 80,
        base_path=parsed.path.rstrip("/"),
    )


def auth_header(user: str | None, password: str | None) -> str | None:
    if user is None:
        return None
    token = base64.b64encode(f"{user}:{password or ''}".encode()).decode()
    return "Basic " + token


def firmware_path_for_env(env: str) -> pathlib.Path:
    return pathlib.Path(".pio") / "build" / env / "firmware.bin"


def run_build(env: str) -> None:
    print(f"building PlatformIO env {env}...")
    subprocess.run(["pio", "run", "-e", env], check=True)


def validate_firmware(path: pathlib.Path) -> int:
    if not path.exists():
        die(f"firmware not found: {path}")
    size = path.stat().st_size
    if size <= 0:
        die(f"firmware is empty: {path}")
    with path.open("rb") as f:
        first = f.read(1)
    if first != b"\xe9":
        die(f"{path} does not look like an ESP32 application image")
    return size


def make_upload_payload(path: pathlib.Path, compression: str | None) -> UploadPayload:
    raw = path.read_bytes()

    if compression is None or compression == "none":
        return UploadPayload(
            filename=path.name,
            data=raw,
            raw_size=len(raw),
            wire_size=len(raw),
            encoding="plain",
        )

    if compression != "zlib":
        die(f"unsupported compression: {compression}")

    compressed = zlib.compress(raw, level=6)
    return UploadPayload(
        filename=path.name + ".zlib",
        data=compressed,
        raw_size=len(raw),
        wire_size=len(compressed),
        encoding="zlib",
    )


def detect_upload_compression(
    target: Target,
    *,
    auth: str | None,
    timeout: float,
) -> str:
    try:
        status, body = request_json(
            target, "GET", "/api/ota", auth=auth, timeout=timeout
        )
    except (OSError, http.client.HTTPException, socket.timeout) as error:
        print(f"compression autodetect failed: {error}; using plain")
        return "none"

    if status >= 400:
        print(f"compression autodetect failed: HTTP {status}; using plain")
        return "none"

    encodings = body.get("upload_encodings")
    if isinstance(encodings, list) and "zlib" in encodings:
        return "zlib"

    return "none"


def make_connection(target: Target, timeout: float) -> http.client.HTTPConnection:
    return http.client.HTTPConnection(target.host, target.port, timeout=timeout)


def api_path(target: Target, path: str, query: dict[str, Any] | None = None) -> str:
    result = target.base_path + path
    if query:
        result += "?" + urlencode(query)
    return result


def request_json(
    target: Target,
    method: str,
    path: str,
    *,
    query: dict[str, Any] | None = None,
    auth: str | None,
    timeout: float,
) -> tuple[int, dict[str, Any]]:
    headers = {"Accept": "application/json"}
    if auth:
        headers["Authorization"] = auth
    conn = make_connection(target, timeout)
    try:
        conn.request(method, api_path(target, path, query), headers=headers)
        response = conn.getresponse()
        raw = response.read()
        try:
            body = json.loads(raw.decode("utf-8") if raw else "{}")
        except json.JSONDecodeError:
            body = {"raw": raw.decode("utf-8", errors="replace")}
        return response.status, body
    finally:
        conn.close()


def describe_ota_error(status: int, body: dict[str, Any]) -> str:
    reason = body.get("last_error") or body.get("error") or body.get("raw")
    if reason:
        return f"HTTP {status}: {reason}"
    return f"HTTP {status}"


def prepare_upload(
    target: Target,
    *,
    payload: UploadPayload,
    auth: str | None,
    timeout: float,
    prepare_timeout: float,
) -> None:
    query = {"size": str(payload.raw_size)}
    if payload.encoding != "plain":
        query["encoding"] = payload.encoding
        query["wire_size"] = str(payload.wire_size)

    print(f"preparing HTTP OTA for {format_bytes(payload.raw_size)}...")
    status, body = request_json(
        target,
        "POST",
        "/api/ota/prepare",
        query=query,
        auth=auth,
        timeout=timeout,
    )
    if status not in (200, 202):
        die("prepare failed: " + describe_ota_error(status, body))

    deadline = time.monotonic() + prepare_timeout
    while True:
        if body.get("http_prepared"):
            partition = body.get("partition") or "--"
            print(f"prepared: partition={partition}")
            return
        if body.get("last_error"):
            die(f"prepare failed: {body['last_error']}")
        if not body.get("http_prepare_pending"):
            die("prepare did not enter pending/prepared state")
        if time.monotonic() >= deadline:
            die("prepare timed out")
        time.sleep(0.25)
        status, body = request_json(
            target, "GET", "/api/ota", auth=auth, timeout=timeout
        )
        if status >= 400:
            die("status poll failed: " + describe_ota_error(status, body))


def print_progress(sent: int, total: int, *, force: bool = False) -> None:
    if total <= 0:
        return
    percent = int((sent * 100) / total)
    now = time.monotonic()
    last_percent = getattr(print_progress, "_last_percent", -1)
    last_time = getattr(print_progress, "_last_time", 0.0)
    if not force and percent == last_percent and now - last_time < 0.5:
        return
    setattr(print_progress, "_last_percent", percent)
    setattr(print_progress, "_last_time", now)
    sys.stdout.write(
        f"\ruploading: {percent:3d}% "
        f"({format_bytes(sent)} / {format_bytes(total)})"
    )
    sys.stdout.flush()


def upload_multipart(
    target: Target,
    *,
    payload: UploadPayload,
    auth: str | None,
    timeout: float,
    chunk_size: int,
) -> dict[str, Any]:
    boundary = "----aircannect-" + uuid.uuid4().hex
    filename = payload.filename
    prefix = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; '
        f'filename="{filename}"\r\n'
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
    ).encode("utf-8")
    suffix = f"\r\n--{boundary}--\r\n".encode("ascii")
    content_length = len(prefix) + payload.wire_size + len(suffix)

    headers = {
        "Accept": "application/json",
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Content-Length": str(content_length),
    }
    if auth:
        headers["Authorization"] = auth

    conn = make_connection(target, timeout)
    try:
        query = {"size": str(payload.raw_size)}
        if payload.encoding != "plain":
            query["encoding"] = payload.encoding
            query["wire_size"] = str(payload.wire_size)

        conn.putrequest(
            "POST",
            api_path(target, "/api/ota/upload", query),
            skip_host=False,
            skip_accept_encoding=True,
        )
        for key, value in headers.items():
            conn.putheader(key, value)
        conn.endheaders()
        conn.send(prefix)

        print_progress(0, payload.wire_size, force=True)
        sent = 0
        for offset in range(0, payload.wire_size, chunk_size):
            chunk = payload.data[offset:offset + chunk_size]
            conn.send(chunk)
            sent += len(chunk)
            print_progress(sent, payload.wire_size)
        conn.send(suffix)
        print_progress(payload.wire_size, payload.wire_size, force=True)
        print()

        response = conn.getresponse()
        raw = response.read()
        try:
            body = json.loads(raw.decode("utf-8") if raw else "{}")
        except json.JSONDecodeError:
            body = {"raw": raw.decode("utf-8", errors="replace")}
        if response.status >= 300:
            die("upload failed: " + describe_ota_error(response.status, body))
        return body
    finally:
        conn.close()


def wait_for_reboot(
    target: Target,
    *,
    auth: str | None,
    timeout: float,
    reboot_timeout: float,
) -> None:
    print("waiting for reboot/API...")
    deadline = time.monotonic() + reboot_timeout
    saw_down = False
    while time.monotonic() < deadline:
        time.sleep(1.0)
        try:
            status, body = request_json(
                target, "GET", "/api/ota", auth=auth, timeout=timeout
            )
        except (OSError, http.client.HTTPException, socket.timeout):
            saw_down = True
            continue
        if status == 200:
            if saw_down:
                print("device API is back")
                return
            if not body.get("reboot_pending") and not body.get("http_ready"):
                print("device API is reachable")
                return
    die("timed out waiting for device API after upload", code=2)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Flash AirCANnect firmware through HTTP OTA."
    )
    parser.add_argument(
        "target",
        nargs="?",
        default=DEFAULT_HOST,
        help="device host, IP, or http:// URL (default: aircannect)",
    )
    parser.add_argument(
        "-e",
        "--env",
        default=DEFAULT_ENV,
        help=f"PlatformIO environment (default: {DEFAULT_ENV})",
    )
    parser.add_argument(
        "-f",
        "--file",
        type=pathlib.Path,
        help="firmware .bin path (default: .pio/build/<env>/firmware.bin)",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="run pio build for --env before flashing",
    )
    parser.add_argument(
        "--compress",
        nargs="?",
        const="zlib",
        default="auto",
        choices=("auto", "zlib", "none"),
        help=(
            "transport compression: auto probes target support, zlib forces "
            "compressed upload, none forces plain upload (bare --compress "
            "means zlib)"
        ),
    )
    parser.add_argument(
        "-u",
        "--user",
        default=os.environ.get("AIRCANNECT_HTTP_USER", DEFAULT_USER),
        help="HTTP auth user; use --no-auth to omit Authorization",
    )
    parser.add_argument(
        "-p",
        "--password",
        default=os.environ.get("AIRCANNECT_HTTP_PASSWORD", DEFAULT_PASSWORD),
        help="HTTP auth password; can also use AIRCANNECT_HTTP_PASSWORD",
    )
    parser.add_argument(
        "--no-auth",
        action="store_true",
        help="do not send HTTP Basic Authorization",
    )
    parser.add_argument(
        "--no-wait",
        action="store_true",
        help="do not wait for the device API after upload",
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--prepare-timeout", type=float, default=20.0)
    parser.add_argument("--reboot-timeout", type=float, default=90.0)
    parser.add_argument("--chunk-size", type=int, default=16 * 1024)
    args = parser.parse_args()

    if args.chunk_size <= 0:
        die("--chunk-size must be positive")

    if args.build:
        run_build(args.env)

    firmware = args.file or firmware_path_for_env(args.env)
    size = validate_firmware(firmware)
    target = parse_target(args.target)
    authorization = None if args.no_auth else auth_header(args.user, args.password)

    print(
        f"target: http://{target.host}:{target.port}{target.base_path or ''}"
    )
    print(f"firmware: {firmware} ({format_bytes(size)})")

    compression = args.compress
    if compression == "auto":
        compression = detect_upload_compression(
            target, auth=authorization, timeout=args.timeout
        )

    payload = make_upload_payload(firmware, compression)

    if payload.encoding != "plain":
        ratio = payload.wire_size / payload.raw_size * 100.0
        print(
            f"transport: {payload.encoding} "
            f"{format_bytes(payload.wire_size)} ({ratio:.1f}% of raw)"
        )
    else:
        print("transport: plain")

    prepare_upload(
        target,
        payload=payload,
        auth=authorization,
        timeout=args.timeout,
        prepare_timeout=args.prepare_timeout,
    )
    body = upload_multipart(
        target,
        payload=payload,
        auth=authorization,
        timeout=max(args.timeout, 60.0),
        chunk_size=args.chunk_size,
    )
    partition = body.get("partition") or "--"
    print(
        f"upload complete: bytes={body.get('bytes', size)} "
        f"wire_bytes={body.get('wire_bytes', payload.wire_size)} "
        f"partition={partition}"
    )
    if not args.no_wait:
        wait_for_reboot(
            target,
            auth=authorization,
            timeout=args.timeout,
            reboot_timeout=args.reboot_timeout,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
