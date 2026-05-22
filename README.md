# AirCANnect

ESP32 bridge for ResMed AirSense 11 / AirCurve 11 CPAP.

![AirCANnect dashboard](docs/screenshots/aircannect-dashboard.png)

## What it does

- **Web UI** 
  - live dashboard with AS11 status and identity
  - read/write all therapy settings
  - live pressure / flow / leak / SpO2 charts
- **Time sync**
  - NTP-first with AS11 clock fallback
  - Optional AirSense time synchronization - fixes RTC drift issue
- **ResMed OTA**
  - flash AirSense firmware from web UI or CLI. \
    (Autodetects firmware format (raw or .abc container) and target memory block)
- **Raw TCP bridge**
  - send commands to AirSense over WiFi. \
    (one JSON-RPC payload per line, compatible with `as11_config.py` and other host tooling.)
- **Multi-profile Wi-Fi**
  - up to four STA profiles, BSSID-targeted roaming, SoftAP auto-fallback or forced always-on.
- **Oximetry**
  - use supported BLE oximeters or UDP sources with AirSense 11 HR/SpO2 recording
  - currently supported: O2Ring, O2Ring-S, Checkme O2, Nonin 3150, generic PLX/HR sensors


## Planned

- **Live therapy data sinks** - HTTP, SMB, SleepHQ, MQTT... Plugin sinks subscribe through the stream broker; bounded per-consumer queues
- **Local EDF capture** - record the active therapy session into set of EDF files on own SD card, indexed for remote retrieval over LAN.
- **BLE provisioning** - replace SoftAP-only first-run with a BLE service for Wi-Fi scan, multi-profile setup, and basic device config from a phone.

## First setup

1. Prepare the hardware (docs coming soon)
2. Flash: `pio run -e xiao-esp32s3-plus-sdmmc4 -t upload`
3. Open `http://aircannect/` (default login: `admin` / `aircannect`).

Without a stored Wi-Fi profile, the device starts SoftAP `AirCANnect_XXXXXX` (password `aircannect`) so you can join it directly and configure Wi-Fi from the web UI at `http://192.168.4.1/`. After the first STA profile is added, SoftAP stays as recovery fallback (configurable to forced-always-on if you want both).

On SD-enabled builds, first-boot provisioning can also be done by placing
`config.txt` in the card root. AirCANnect reads `key=value` lines during boot,
then renames the file to `config.ok`. See the full
[configuration reference](docs/configuration.md). Useful keys include:

```ini
host=aircannect
ssid_0=your-network
pass_0=your-password
wifi_ctry=01
tz=CET-1CEST,M3.5.0,M10.5.0/3
http_user=admin
http_pass=aircannect
```

For multiple Wi-Fi profiles, use `ssid_0` / `pass_0` through
`ssid_3` / `pass_3`.

## Build profiles

- `xiao-esp32s3-plus-sdmmc4` *(default)* - XIAO ESP32-S3 Plus, 4-bit microSD on the exposed non-strapping GPIOs.
- `xiao-esp32s3-plus-sdmmc1` - same board, 1-bit SDMMC.
- `xiao-esp32s3-plus-spisd` - same board, SPI-mode SD fallback.
- `xiao-esp32s3-plus` - no SD; PSRAM still available for stream pool and response buffers.

<!--
## Related tools

[airbreak-plus](https://github.com/m-kozlowski/airbreak-plus/tree/master/python/) has Python tooling that should use the generic AirCANnect host transport:

- `python/as11_config.py -p tcp:aircannect` - read/write AS11 settings, run RPC calls, follow streams.
- `python/as11_flash.py -p tcp:aircannect` - upload ResMed firmware through the same transport.

Both also support `-p /dev/ttyUSB0` for direct serial CAN tooling on hardware that exposes it.
-->

## Screenshots

| Live charts | Clinical Settings |
|---|---|
| ![Live](docs/screenshots/charts.png) | ![Clinical](docs/screenshots/clinical.png) |

| ResMed OTA | Oximetry |
|---|---|
| ![OTA](docs/screenshots/ota.png) | ![Oximetry](docs/screenshots/oximetry.png) |

## License

AGPL v3. See [LICENSE](LICENSE).
