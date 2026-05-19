# Configuration Reference

## Device

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `host` | Hostname, 1-63 alnum/hyphen chars, not starting or ending with hyphen | `aircannect` | yes | Device hostname and SoftAP SSID prefix. |

## Wi-Fi Profiles

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `ssid_0` ... `ssid_3` | SSID string, or empty | empty | yes | Wi-Fi profile SSID slots 0 through 3. Provisioned SSID slots replace the stored profile list; `ssid_0=` with no other SSIDs clears Wi-Fi profiles. |
| `pass_0` ... `pass_3` | Password string, or empty | empty | yes | Wi-Fi profile password slots 0 through 3. Empty or missing password marks that profile open. |

## Wi-Fi Behavior

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `wifi_ctry` | `01`, or two-letter ISO country code | `01` | yes | Wi-Fi regulatory country. `01` is the worldwide-safe setting. |
| `softap_mode` | `auto`, `forced` | `auto` | yes | `auto` keeps SoftAP as fallback/recovery. `forced` keeps SoftAP running alongside STA. |

Roaming is implicit when more than one Wi-Fi profile exists.

## Network Services

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `tcp_en` | boolean | `on` | yes | Enable the raw AS11 TCP bridge. |
| `tcp_port` | TCP port `1` to `65535` | `39011` | yes | Raw AS11 TCP bridge port. |
| `telnet_en` | boolean | `on` | yes | Enable the telnet management console. |
| `telnet_port` | TCP port `1` to `65535` | `23` | yes | Telnet management console port. |

## HTTP Access

Empty `http_user` and `http_pass` mean open HTTP access.

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `http_user` | ASCII string, max 64 chars, or empty | `admin` | yes | Web UI/API username. |
| `http_pass` | ASCII string, max 64 chars, or empty | `aircannect` | yes | Web UI/API password. |
| `auth_wl` | Empty, IPv4 entries/ranges separated by commas | empty | yes | IP/range whitelist that bypasses HTTP auth. |

## Time

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `tz` | POSIX timezone string, max 64 chars | `UTC` | yes | Local timezone used for UI/CLI display. AS11 RPC time remains UTC. |
| `resmed_time` | boolean | `off` | yes | When NTP is synced and therapy is idle, push ESP time to the ResMed device. |

## ESP32 OTA

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `ota_pass` | ASCII string up to 64 chars, or empty | `aircannect` | yes | ArduinoOTA password. Empty value allows ArduinoOTA without authorization. |

## Logging

| Key | Values | Default | Provisioning | Description |
| --- | --- | --- | --- | --- |
| `syslog_en` | boolean | `off` | yes | Enable UDP syslog forwarding. |
| `syslog_host` | Empty, or IPv4 address | empty | yes | Syslog destination host. |
| `syslog_port` | UDP port `1` to `65535` | `514` | yes | Syslog destination port. |
| `log0` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `GENERAL` category log level. |
| `log1` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `CAN` category log level. |
| `log2` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `RPC` category log level. |
| `log3` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `TCP` category log level. |
| `log4` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `CLI` category log level. |
| `log5` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `WIFI` category log level. |
| `log6` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `STREAM` category log level. |
| `log7` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | no | `OTA` category log level. |

Per-category log levels are currently changed from the management console with
`log level`, not from `config.txt`.
