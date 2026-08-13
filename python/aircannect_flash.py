#!/usr/bin/env python3
"""Flash AirCANnect firmware through the device HTTP OTA endpoint.

This deliberately uses the same HTTP API as the web UI instead of PlatformIO
espota. That makes it usable from WSL/NAT environments where espota callbacks
are unreliable.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import threading
import time
import uuid
import zlib
from dataclasses import dataclass
from typing import Any, Callable
from urllib.parse import urlencode, urlparse


DEFAULT_ENV = "xiao-esp32s3-plus-sdmmc4"
DEFAULT_HOST = "aircannect"
DEFAULT_USER = "admin"
DEFAULT_PASSWORD = "aircannect"


class FlashError(Exception):
    def __init__(self, message: str, code: int = 1) -> None:
        super().__init__(message)
        self.code = code


@dataclass
class OutputContext:
    prefix: str = ""
    target_label: str = ""
    inline_status: bool = False
    last_status: str = ""
    last_status_time: float = 0.0
    upload_percent: int = -1
    upload_time: float = 0.0


_OUTPUT_LOCK = threading.Lock()
_OUTPUT_CONTEXT = threading.local()


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


class MultiTargetOutput:
    def __init__(self, labels: list[str]) -> None:
        self.labels = labels
        self.label_width = max(len(label) for label in labels)
        self.statuses = {label: "queued" for label in labels}
        self.tty = sys.stdout.isatty()
        self.lines_drawn = 0

    def start(self) -> None:
        if self.tty:
            self._render()
            return

        print("targets: " + ", ".join(self.labels), flush=True)

    def update(self, label: str, message: str, *, error: bool = False) -> None:
        self.statuses[label] = "ERROR: " + message if error else message
        if self.tty:
            self._render()
            return

        stream = sys.stderr if error else sys.stdout
        print(f"[{label}] {message}", file=stream, flush=True)

    def finish(self) -> None:
        self.lines_drawn = 0

    def _render(self) -> None:
        if self.lines_drawn:
            sys.stdout.write(f"\x1b[{self.lines_drawn}A")

        for label in self.labels:
            sys.stdout.write(
                "\r\x1b[2K"
                f"{label:<{self.label_width}}  {self.statuses[label]}\n"
            )
        sys.stdout.flush()
        self.lines_drawn = len(self.labels)


_MULTI_OUTPUT: MultiTargetOutput | None = None


def die(message: str, code: int = 1) -> None:
    raise FlashError(message, code)


def set_output_context(prefix: str = "", target_label: str = "") -> None:
    _OUTPUT_CONTEXT.value = OutputContext(
        prefix=prefix,
        target_label=target_label,
    )


def output_context() -> OutputContext:
    context = getattr(_OUTPUT_CONTEXT, "value", None)
    if context is None:
        context = OutputContext()
        _OUTPUT_CONTEXT.value = context
    return context


def emit(message: str = "", *, error: bool = False) -> None:
    context = output_context()
    stream = sys.stderr if error else sys.stdout

    with _OUTPUT_LOCK:
        if _MULTI_OUTPUT and context.target_label:
            _MULTI_OUTPUT.update(
                context.target_label,
                message,
                error=error,
            )
            return

        if context.inline_status:
            sys.stdout.write("\n")
            context.inline_status = False
        print(context.prefix + message, file=stream, flush=True)


def update_status(message: str, *, force: bool = False) -> None:
    context = output_context()
    now = time.monotonic()

    if _MULTI_OUTPUT and context.target_label:
        if not force and now - context.last_status_time < 1.0:
            return

        with _OUTPUT_LOCK:
            _MULTI_OUTPUT.update(context.target_label, message)
    elif context.prefix:
        if not force and now - context.last_status_time < 1.0:
            return

        with _OUTPUT_LOCK:
            print(context.prefix + message, flush=True)
    else:
        with _OUTPUT_LOCK:
            padding = " " * max(0, len(context.last_status) - len(message))
            sys.stdout.write("\r" + message + padding)
            sys.stdout.flush()
            context.inline_status = True

    context.last_status = message
    context.last_status_time = now


def finish_status() -> None:
    context = output_context()
    if not context.inline_status:
        return

    with _OUTPUT_LOCK:
        sys.stdout.write("\n")
        sys.stdout.flush()
    context.inline_status = False


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


def target_url(target: Target) -> str:
    host = f"[{target.host}]" if ":" in target.host else target.host
    return f"http://{host}:{target.port}{target.base_path}"


def target_label(target: Target) -> str:
    host = f"[{target.host}]" if ":" in target.host else target.host
    port = f":{target.port}" if target.port != 80 else ""
    return f"{host}{port}{target.base_path}"


def parse_targets(values: list[str]) -> list[Target]:
    targets = [parse_target(value) for value in values]
    seen: set[tuple[str, str, int, str]] = set()

    for target in targets:
        identity = (
            target.scheme,
            target.host.lower(),
            target.port,
            target.base_path,
        )
        if identity in seen:
            die(f"duplicate target: {target_url(target)}")
        seen.add(identity)

    return targets


def auth_header(user: str | None, password: str | None) -> str | None:
    if user is None:
        return None
    token = base64.b64encode(f"{user}:{password or ''}".encode()).decode()
    return "Basic " + token


def firmware_path_for_env(env: str) -> pathlib.Path:
    return pathlib.Path(".pio") / "build" / env / "firmware.bin"


def run_build(env: str) -> None:
    emit(f"building PlatformIO env {env}...")
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
        emit(f"compression autodetect failed: {error}; using plain")
        return "none"

    if status >= 400:
        emit(f"compression autodetect failed: HTTP {status}; using plain")
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


def url_source_encoding(requested: str) -> str:
    if requested == "none":
        return "plain"
    return requested


def display_source_url(source_url: str) -> str:
    parsed = urlparse(source_url)
    host = parsed.hostname or ""
    if ":" in host:
        host = f"[{host}]"
    if parsed.port is not None:
        host += f":{parsed.port}"
    return f"{parsed.scheme}://{host}{parsed.path}"


def start_url_update(
    target: Target,
    *,
    source_url: str,
    encoding: str,
    auth: str | None,
    timeout: float,
) -> dict[str, Any]:
    query: dict[str, Any] = {"url": source_url, "encoding": encoding}

    status, capabilities = request_json(
        target, "GET", "/api/ota", auth=auth, timeout=timeout
    )
    if status >= 400:
        die("OTA status failed: " + describe_ota_error(status, capabilities))
    if not capabilities.get("url_update"):
        die("target firmware does not support OTA from URL")
    if encoding not in capabilities.get("upload_encodings", []):
        die(f"target firmware does not support {encoding} OTA images")

    emit(f"source: {display_source_url(source_url)}")
    emit(f"transport: device download, {encoding}")
    status, body = request_json(
        target,
        "POST",
        "/api/ota/url",
        query=query,
        auth=auth,
        timeout=timeout,
    )
    if status not in (200, 202):
        die("URL update rejected: " + describe_ota_error(status, body))
    return body


def wait_for_url_update(
    target: Target,
    *,
    initial: dict[str, Any],
    auth: str | None,
    timeout: float,
    url_timeout: float,
) -> dict[str, Any]:
    emit("device is downloading and installing the image...")
    body = initial
    deadline = time.monotonic() + url_timeout
    last_line = ""
    saw_disconnect = False

    while time.monotonic() < deadline:
        total = int(body.get("total_size") or 0)
        written = int(body.get("bytes") or 0)
        wire_total = int(body.get("wire_total_size") or 0)
        wire_read = int(body.get("wire_bytes") or 0)
        if total:
            line = f"{int(body.get('progress') or 0):3d}% raw {format_bytes(written)}"
            if body.get("encoding") == "zlib" and wire_total:
                line += f", wire {format_bytes(wire_read)} / {format_bytes(wire_total)}"
        elif wire_total:
            line = (
                f"{int(body.get('progress') or 0):3d}% wire "
                f"{format_bytes(wire_read)} / {format_bytes(wire_total)}"
            )
        else:
            line = "resolving source URL"
        if line != last_line:
            update_status(line)
            last_line = line

        if body.get("last_error") and not body.get("url_active"):
            finish_status()
            die(f"URL update failed: {body['last_error']}")
        if body.get("reboot_pending") or body.get("http_ready"):
            finish_status()
            return body

        time.sleep(0.5)
        try:
            status, body = request_json(
                target, "GET", "/api/ota", auth=auth, timeout=timeout
            )
        except (OSError, http.client.HTTPException, socket.timeout):
            saw_disconnect = True
            continue
        if status >= 400:
            finish_status()
            die("status poll failed: " + describe_ota_error(status, body))
        if (
            saw_disconnect
            and not body.get("url_active")
            and not body.get("http_prepare_pending")
            and not body.get("http_active")
            and not body.get("last_error")
        ):
            finish_status()
            return body

    finish_status()
    die("timed out waiting for URL update")


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

    emit(f"preparing HTTP OTA for {format_bytes(payload.raw_size)}...")
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
            emit(f"prepared: partition={partition}")
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
    context = output_context()
    last_percent = context.upload_percent
    last_time = context.upload_time
    if percent == last_percent:
        return
    if (
        _MULTI_OUTPUT
        and not _MULTI_OUTPUT.tty
        and not force
        and percent < min(100, last_percent + 25)
    ):
        return
    if (
        not force
        and not (_MULTI_OUTPUT and not _MULTI_OUTPUT.tty)
        and now - last_time < 0.5
    ):
        return
    context.upload_percent = percent
    context.upload_time = now
    update_status(
        f"uploading: {percent:3d}% "
        f"({format_bytes(sent)} / {format_bytes(total)})",
        force=force or bool(_MULTI_OUTPUT and not _MULTI_OUTPUT.tty),
    )


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
        finish_status()

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
    emit("waiting for reboot/API...")
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
                emit("device API is back")
                return
            if not body.get("reboot_pending") and not body.get("http_ready"):
                emit("device API is reachable")
                return
    die("timed out waiting for device API after upload", code=2)


def run_target(
    target: Target,
    *,
    prefix: str,
    operation: Callable[[Target], None],
) -> int:
    label = target_label(target) if _MULTI_OUTPUT else ""
    set_output_context(prefix, label)
    if not _MULTI_OUTPUT:
        emit(f"target: {target_url(target)}")

    try:
        operation(target)
    except FlashError as error:
        finish_status()
        emit(f"error: {error}", error=True)
        return error.code
    except (OSError, http.client.HTTPException, socket.timeout) as error:
        finish_status()
        emit(f"error: {error}", error=True)
        return 1

    return 0


def run_target_operations(
    targets: list[Target], operation: Callable[[Target], None]
) -> int:
    global _MULTI_OUTPUT

    if len(targets) == 1:
        return run_target(targets[0], prefix="", operation=operation)

    _MULTI_OUTPUT = MultiTargetOutput(
        [target_label(target) for target in targets]
    )
    with _OUTPUT_LOCK:
        _MULTI_OUTPUT.start()

    def worker(target: Target) -> int:
        return run_target(
            target,
            prefix=f"[{target_label(target)}] ",
            operation=operation,
        )

    try:
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=len(targets),
            thread_name_prefix="aircannect-flash",
        ) as executor:
            results = list(executor.map(worker, targets))
    finally:
        with _OUTPUT_LOCK:
            _MULTI_OUTPUT.finish()
        _MULTI_OUTPUT = None

    succeeded = sum(result == 0 for result in results)
    emit(f"completed: {succeeded}/{len(targets)} targets")
    return max(results)


def validate_source_url(source_url: str) -> None:
    parsed_source = urlparse(source_url)
    try:
        source_port = parsed_source.port
    except ValueError:
        die("--url contains an invalid port")
    if source_port == 0:
        die("--url contains an invalid port")
    if (
        parsed_source.scheme not in ("http", "https")
        or not parsed_source.hostname
    ):
        die("--url must be an HTTP or HTTPS URL")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Flash AirCANnect firmware through HTTP OTA."
    )
    parser.add_argument(
        "targets",
        nargs="*",
        default=[DEFAULT_HOST],
        metavar="TARGET",
        help=(
            "one or more device hosts, IPs, or http:// URLs; multiple "
            "targets are flashed in parallel (default: aircannect)"
        ),
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
        "--url",
        dest="source_url",
        help="ask the device to download and install this HTTP(S) URL",
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
            "means zlib); URL auto lets the device detect the image format"
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
    parser.add_argument("--url-timeout", type=float, default=600.0)
    parser.add_argument("--reboot-timeout", type=float, default=90.0)
    parser.add_argument("--chunk-size", type=int, default=16 * 1024)
    args = parser.parse_args()

    if args.chunk_size <= 0:
        die("--chunk-size must be positive")

    targets = parse_targets(args.targets)
    authorization = None if args.no_auth else auth_header(args.user, args.password)

    if args.source_url:
        if args.file or args.build:
            die("--url cannot be combined with --file or --build")
        validate_source_url(args.source_url)

        encoding = url_source_encoding(args.compress)

        def install_url(target: Target) -> None:
            body = start_url_update(
                target,
                source_url=args.source_url,
                encoding=encoding,
                auth=authorization,
                timeout=args.timeout,
            )
            body = wait_for_url_update(
                target,
                initial=body,
                auth=authorization,
                timeout=args.timeout,
                url_timeout=args.url_timeout,
            )
            emit(
                f"update complete: bytes={body.get('bytes', 0)} "
                f"wire_bytes={body.get('wire_bytes', 0)} "
                f"partition={body.get('partition') or '--'}"
            )
            if not args.no_wait:
                wait_for_reboot(
                    target,
                    auth=authorization,
                    timeout=args.timeout,
                    reboot_timeout=args.reboot_timeout,
                )

        return run_target_operations(targets, install_url)

    if args.build:
        run_build(args.env)

    firmware = args.file or firmware_path_for_env(args.env)
    size = validate_firmware(firmware)
    emit(f"firmware: {firmware} ({format_bytes(size)})")

    payloads: dict[str, UploadPayload] = {}
    if args.compress in ("auto", "none"):
        payloads["none"] = make_upload_payload(firmware, "none")
    if args.compress in ("auto", "zlib"):
        payloads["zlib"] = make_upload_payload(firmware, "zlib")

    def upload_firmware(target: Target) -> None:
        compression = args.compress
        if compression == "auto":
            compression = detect_upload_compression(
                target, auth=authorization, timeout=args.timeout
            )

        payload = payloads[compression]
        if payload.encoding != "plain":
            ratio = payload.wire_size / payload.raw_size * 100.0
            emit(
                f"transport: {payload.encoding} "
                f"{format_bytes(payload.wire_size)} "
                f"({ratio:.1f}% of raw)"
            )
        else:
            emit("transport: plain")

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
        emit(
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

    return run_target_operations(targets, upload_firmware)


if __name__ == "__main__":
    try:
        exit_code = main()
    except FlashError as error:
        emit(f"error: {error}", error=True)
        exit_code = error.code
    raise SystemExit(exit_code)
