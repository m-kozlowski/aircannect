# AirCANnect Host Transport Protocol

This document specifies the protocol a host-side AS11 tool should implement to
use AirCANnect as a network transport. The intended clients are tools such as
`as11_config.py` and `as11_flash.py` in the airbreak project.

The transport is deliberately generic. AirCANnect does not expose a separate
flashing protocol for host tools; flashing is just AS11 JSON-RPC over the same
line transport used for configuration, reads, writes, subscriptions, and
streams.

## Protocol Version

This is AirCANnect host transport v1.

Version 1 is a raw AS11 JSON-RPC line transport over TCP:

```text
host -> AirCANnect: one complete AS11 JSON-RPC payload per LF-terminated line
AirCANnect -> host: one complete AS11 JSON payload per LF-terminated line
```

## Connection

- Default port: `39011`.
- The port is configurable through AirCANnect's `tcp` config key.
- Transport: plain TCP.
- No TLS.
- No HTTP.
- No authentication on this port.
- No greeting banner.
- No capability exchange.
- Recommended socket option: `TCP_NODELAY`.

The connection is expected to be used only on a trusted LAN or direct recovery
network. AirCANnect's HTTP/telnet authorization does not protect this raw TCP
bridge.

If the bridge is disabled or the maximum client count is reached, the client
may see connection refusal, immediate close, or the overload line
`ERR: max clients` before close. Treat any non-JSON line as a transport error.

## Framing

Each message is one UTF-8 JSON document followed by `\n`.

Clients may send `\r\n`; AirCANnect ignores `\r` and trims the completed input
line. Clients should send compact JSON and should not rely on leading or
trailing whitespace.

Current AirCANnect limits:

| Item | Current limit |
| --- | --- |
| TCP clients | 4 |
| Host request line | 2048 bytes |
| Reassembled AS11 datagram payload | 8192 bytes |
| Per-client outbound queue | 24 lines |

The request-line limit matters for `UpgradeDataBlock`: a 500-byte raw block
encoded as `AsciiHex` fits comfortably below the current 2048-byte line limit.

## Payload Ownership

AirCANnect does not parse or rewrite host-submitted raw TCP requests before
sending them to AS11. The host owns:

- JSON-RPC `id` generation.
- JSON-RPC version selection.
- Method names.
- Params shape.
- Request timeouts.
- Matching responses to requests.
- Ignoring unrelated responses and notifications.

AirCANnect broadcasts AS11 RPC responses and notifications to all connected raw
TCP clients. A client can therefore receive:

- the response to its own request,
- responses caused by another raw TCP client,
- responses caused by AirCANnect console or HTTP actions,
- AS11 notifications such as `StreamData` or `EventNotification`.

Clients must match responses by `id` and ignore or dispatch everything else.

Because AirCANnect also has internal scheduler requests, host clients should use
large random numeric request IDs and avoid reuse during a connection. This keeps
collisions with AirCANnect-owned request IDs practically impossible.

## Error Model

There is no AirCANnect-level acknowledgement for a submitted raw TCP request.

If AirCANnect cannot enqueue the CAN datagram, it logs the failure internally
but does not send a local JSON error on the raw TCP port. From the host
transport point of view this is a timeout.

Host clients should:

- timeout each request,
- reconnect on socket errors,
- tolerate unrelated messages while waiting,
- treat non-JSON lines as transport errors,
- retry only when the AS11 operation is safe to retry.

Suggested default timeout for ordinary RPC calls: 8 seconds.

Suggested timeout for `CheckUpgradeFile` and apply operations: 120 seconds.

## JSON-RPC Versions

AirCANnect does not correct the `jsonrpc` value for raw TCP requests. Host tools
must use the AS11 method metadata they already use on other transports.

Known method versions used by AirCANnect's own request builder:

| Method | JSON-RPC version |
| --- | --- |
| `GetVersion` | `2.0` |
| `EnterMaskFit` | `2.0` |
| `SetDateTime` | `1.1` |
| `ApplyUpgrade` | `1.1` |
| `GenerateAuthCode` | `1.1` |
| Other known AS11 methods | `1.0` unless airbreak metadata says otherwise |

This table is a convenience, not a replacement for the airbreak AS11 method
catalog.

## Basic Call Flow

Example request:

```json
{"jsonrpc":"1.0","method":"Get","params":["_PNA","_SRN","_SID"],"id":901234001}
```

Wire form:

```text
{"jsonrpc":"1.0","method":"Get","params":["_PNA","_SRN","_SID"],"id":901234001}\n
```

Possible response:

```json
{"jsonrpc":"2.0","id":901234001,"result":{"_PNA":"AirSense 11 AutoSet","_SRN":"23252400723","_SID":"SW04600.15.8.4.0.791777c3b"}}
```

Client algorithm:

1. Open TCP connection.
2. Generate a large random numeric request ID.
3. Send one JSON-RPC request line.
4. Read JSON lines until a response with matching `id` arrives or timeout
   expires.
5. Surface `error` responses as AS11 RPC errors.
6. Dispatch or ignore notifications and unrelated responses.

## Streams And Events

Subscriptions use normal AS11 JSON-RPC methods over the same transport.

For stream data, the host sends `StartStream` with AS11 `dataIds`, `intervalMs`,
and related params. AS11 then emits `StreamData` notifications. AirCANnect does
not apply per-client stream filtering on the raw TCP bridge; every connected
raw TCP client receives all AS11 stream notifications that AirCANnect broadcasts.

Clients that start a stream should also stop it when finished if their AS11
flow requires an explicit stop. Clients must continuously drain the socket while
streaming; slow clients can overrun their per-client outbound queue and lose
notifications.

For therapy/activity events, use AS11 `SubscribeEvent` normally. AirCANnect's
own firmware also subscribes to activity events for its dashboard state, but
raw TCP host clients should not depend on AirCANnect's internal subscription.

## Flashing Over The Generic Transport

`as11_flash.py` should use the same host transport as `as11_config.py`.

AirCANnect does not add safety checks to raw TCP flashing calls. A host flasher
must decide whether the device is idle, whether the image is appropriate, and
whether apply is allowed.

Recommended AS11 OTA flow:

1. Build or load the `.abc` payload host-side.
2. Send:

   ```json
   {"jsonrpc":"1.0","method":"InitiateUpgrade","params":{"upgradeFileSize":1966256},"id":901240001}
   ```

3. Read `result.xferBlockSize` if present. Use the smaller of that value and
   the host transport's block policy. AirCANnect's current on-device flow uses
   500 raw bytes per block.
4. For each accepted block, send:

   ```json
   {"jsonrpc":"1.0","method":"UpgradeDataBlock","params":{"fileOffset":0,"encoding":"AsciiHex","data":"..."},"id":901240002}
   ```

   `data` is uppercase or lowercase hexadecimal for the raw block bytes. Wait
   for a successful response before sending the next block.
5. Compute SHA-256 over the exact byte stream accepted by AS11.
6. Send:

   ```json
   {"jsonrpc":"1.0","method":"CheckUpgradeFile","params":{"upgradeFileHash":"HEX_SHA256"},"id":901250001}
   ```

7. If applying immediately is desired, send `ApplyUpgrade` or
   `ApplyAuthenticatedUpgrade` using the airbreak AS11 OTA rules and key
   material.

The host flasher should not use AirCANnect's browser-oriented
`/api/resmed-ota/upload` endpoint for the first generic airbreak transport. That
HTTP endpoint is useful for manual browser uploads and device-side raw-image
staging, but it is not the common transport shared by `as11_config.py` and
`as11_flash.py`.

## Compatibility Tests

A host implementation should pass these minimal checks:

1. Connect to `aircannect:39011` or the configured IP/port.
2. Confirm no banner is received before the first request.
3. Send `GetDateTime` and match the response by `id`.
4. Send `Get` for `_PNA`, `_SRN`, and `_SID`.
5. While waiting for a response, ignore unrelated notifications and responses.
6. Start a short stream sample, continuously drain notifications, then stop or
   disconnect cleanly according to the AS11 stream method used.
7. For flashing, send no block larger than the negotiated AS11 block size and
   no raw-TCP request line larger than the transport line limit.

