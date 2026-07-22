# Configuration Reference

Persistent AirCANnect configuration keys.

Boolean values accept `on`/`off`, `yes`/`no`, `true`/`false`, `1`/`0`, and
`enabled`/`disabled`.

## Device

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `host` | Hostname, 1-63 alnum/hyphen chars, not starting or ending with hyphen | `aircannect` | Device hostname and SoftAP SSID prefix. |
| `edf_cap` | boolean | on when the build has SD storage | Enable AS11-style EDF capture on SD card. |

## Wi-Fi Profiles

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `ssid_0` ... `ssid_3` | SSID string, or empty | empty | Wi-Fi profile SSID slots 0 through 3. Provisioned SSID slots replace the stored profile list; `ssid_0=` with no other SSIDs clears Wi-Fi profiles. |
| `pass_0` ... `pass_3` | Password string, or empty | empty | Wi-Fi profile password slots 0 through 3. Empty or missing password marks that profile open. |

## Wi-Fi Behavior

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `wifi_ctry` | `01`, or two-letter ISO country code | `01` | Wi-Fi regulatory country. `01` is the worldwide-safe setting. |
| `softap_mode` | `auto`, `forced` | `auto` | `auto` uses SoftAP as fallback/recovery. `forced` keeps SoftAP running alongside STA. |

Roaming is implicit when more than one Wi-Fi profile exists.

## Network Services

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `tcp_en` | boolean | `on` | Enable the raw AS11 TCP bridge. |
| `tcp_port` | TCP port `1` to `65535` | `39011` | Raw AS11 TCP bridge port. |
| `telnet_en` | boolean | `on` | Enable the telnet management console. |
| `telnet_port` | TCP port `1` to `65535` | `23` | Telnet management console port. |

## Access

Empty `http_user` and `http_pass` mean open access for HTTP and telnet.

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `http_user` | ASCII string, max 64 chars, or empty | `admin` | Web UI/API username. |
| `http_pass` | ASCII string, max 64 chars, or empty | `aircannect` | Web UI/API password. |
| `auth_wl` | Empty, `*`, IPv4 addresses, CIDR ranges, or IPv4 start-end ranges separated by commas | empty | IP/range whitelist that bypasses HTTP and telnet auth. |
| `ota_pass` | ASCII string up to 64 chars, or empty | `aircannect` | ArduinoOTA password. Empty value allows ArduinoOTA without authorization. |

## Updates

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `update_url` | HTTP(S) URL, or empty | GitHub latest-release manifest | Release manifest checked for compatible AirCANnect firmware. Empty disables update checks. |

AirCANnect checks about one minute after IPv4 becomes available and then every
six hours. Checks wait until the device is idle: no therapy, foreground report
work, storage export, or firmware update may be active. They only report a
newer compatible release; firmware installation remains an explicit user
action from the OTA tab or the `ota install` CLI command.

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

## SMB Sync

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `smb_ep` | `//host/share/path` | empty | Destination SMB share/path. |
| `smb_user` | SMB username | empty | SMB username. |
| `smb_pass` | SMB password | empty | SMB password. |

## SleepHQ Sync

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `shq_id` | SleepHQ OAuth client ID | empty | OAuth client ID. |
| `shq_secret` | SleepHQ OAuth client secret | empty | OAuth client secret. |
| `shq_team` | Numeric SleepHQ team ID | empty | Optional team ID. |
| `shq_device` | Numeric SleepHQ device ID, or empty | empty | Optional SleepHQ device type override. |

## Logging

| Key | Values | Default | Description |
| --- | --- | --- | --- |
| `syslog_en` | boolean | `off` | Enable UDP syslog forwarding. |
| `syslog_host` | Empty, or IPv4 address | empty | Syslog destination host. |
| `syslog_port` | UDP port `1` to `65535` | `514` | Syslog destination port. |
| `file_log_en` | boolean | on when the build has SD storage | Enable persistent SD-card log files when storage is available. |
| `log_general` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | General log category level. |
| `log_can` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | CAN log category level. |
| `log_rpc` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | RPC log category level. |
| `log_tcp` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | TCP bridge log category level. |
| `log_cli` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | CLI log category level. |
| `log_wifi` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Wi-Fi log category level. |
| `log_stream` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Live stream log category level. |
| `log_ota` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | OTA log category level. |
| `log_oxi` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Oximetry log category level. |
| `log_storage` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Storage log category level. |
| `log_export` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Export task log category level. |
| `log_report` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Report log category level. |
| `log_edf` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | EDF capture/report log category level. |
| `log_config` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | Configuration log category level. |
| `log_sleephq` | `ERROR`, `WARN`, `INFO`, `DEBUG` | `INFO` | SleepHQ sync log category level. |

The management console also has a separate convenience command for interactive
logging changes:

```text
log level LEVEL
log level CATEGORY LEVEL
```
