# Configuration Reference

Persistent AirCANnect configuration keys.

Boolean values accept `on`/`off`, `yes`/`no`, `true`/`false`, `1`/`0`, and
`enabled`/`disabled`.

## Device

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `host` | Hostname, 1-63 alnum/hyphen chars, not starting or ending with hyphen | `aircannect` | Device hostname and SoftAP SSID prefix. |

## Wi-Fi Profiles

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `ssid_0` ... `ssid_3` | SSID string, or empty | empty | Wi-Fi profile SSID slots 0 through 3. Provisioned SSID slots replace the stored profile list; `ssid_0=` with no other SSIDs clears Wi-Fi profiles. |
| `pass_0` ... `pass_3` | Password string, or empty | empty | Wi-Fi profile password slots 0 through 3. Empty or missing password marks that profile open. |

## Wi-Fi Behavior

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `wifi_ctry` | `01`, or two-letter ISO country code | `01` | Wi-Fi regulatory country. `01` is the worldwide-safe setting. |
| `softap_mode` | `auto`, `forced` | `auto` | `auto` keeps SoftAP as fallback/recovery. `forced` keeps SoftAP running alongside STA. |

Roaming is implicit when more than one Wi-Fi profile exists.

## Network Services

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `tcp_en` | boolean | `on` | Enable the raw AS11 TCP bridge. |
| `tcp_port` | TCP port `1` to `65535` | `39011` | Raw AS11 TCP bridge port. |
| `telnet_en` | boolean | `on` | Enable the telnet management console. |
| `telnet_port` | TCP port `1` to `65535` | `23` | Telnet management console port. |

## Network Access

Empty `http_user` and `http_pass` mean open access for HTTP and telnet.

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `http_user` | ASCII string, max 64 chars, or empty | `admin` | Web UI/API username. |
| `http_pass` | ASCII string, max 64 chars, or empty | `aircannect` | Web UI/API password. |
| `auth_wl` | Empty, `*`, IPv4 addresses, CIDR ranges, or IPv4 start-end ranges separated by commas | empty | IP/range whitelist that bypasses HTTP and telnet auth. |

## Time

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `tz` | POSIX timezone string, max 64 chars | `UTC` | Local timezone used for UI/CLI display. AS11 RPC time remains UTC. |
| `resmed_time` | boolean | `off` | When NTP is synced and therapy is idle, push ESP time to the ResMed device. |

## Oximetry

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `oxi_en` | boolean | `on` | Enable oximetry input and AirSense recording bridge. |
| `oxi_udp` | UDP port `1` to `65535` | `8025` | UDP source port. Source format is AirBridge-compatible 7-byte packets. |
| `oxi_adv` | `auto`, `manual` | `auto` | `auto` advertises to AirSense while a source is available. `manual` uses explicit `oxi advertise start` / `stop`. |

Known BLE oximeters are stored separately and managed from the Web UI or
`oxi sensor` console commands, not from `config.txt`.

## ESP32 OTA

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `ota_pass` | ASCII string up to 64 chars, or empty | `aircannect` | ArduinoOTA password. Empty value allows ArduinoOTA without authorization. |

## Logging

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `syslog_en` | boolean | `off` | Enable UDP syslog forwarding. |
| `syslog_host` | Empty, or IPv4 address | empty | Syslog destination host. |
| `syslog_port` | UDP port `1` to `65535` | `514` | Syslog destination port. |
| `file_log_en` | boolean | storage build default | Enable persistent SD-card log files when storage is available. |
| `log0` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `GENERAL` category log level. |
| `log1` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `CAN` category log level. |
| `log2` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `RPC` category log level. |
| `log3` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `TCP` category log level. |
| `log4` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `CLI` category log level. |
| `log5` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `WIFI` category log level. |
| `log6` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `STREAM` category log level. |
| `log7` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `OTA` category log level. |
| `log8` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | `OXI` category log level. |

Per-category log levels are currently changed from the management console with
`log level`, not from `config.txt`.
