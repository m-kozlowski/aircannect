# AirCANnect Quickstart

From a freshly-built XIAO ESP32-S3 Plus to a working web UI in a few
minutes. Assumes the [hardware](hardware.md) is wired up.

## 1. Flash

```bash
pio run -e xiao-esp32s3-plus-sdmmc4 -t upload
```

Other build profiles (1-bit SDMMC, SPI SD, no SD) are listed in
[hardware.md](hardware.md#xiao-esp32-s3-plus-pin-assignments).

## 2. First boot

Open the serial console at `921600` baud. The boot banner prints version,
chip, PSRAM, and storage state. Look for:

```
[BOOT] version=
```

near the start. If you see repeated `[CAN] alert:` lines with `bus_error` or
`tx_failed` right after boot, re-check transceiver pin labels,
`CANH` / `CANL` polarity, and ground connections (see
[hardware.md](hardware.md#can-transceiver)).

## 3. Get on Wi-Fi

Two paths.

**SoftAP setup**  With no stored profile the device
brings up SoftAP `AirCANnect_XXXXXX` (password `aircannect`). Join it from
a phone or laptop, open `http://192.168.4.1/`, log in as `admin` /
`aircannect`, and add your home Wi-Fi from the Wi-Fi tab.

**SD provisioning**  On SD-enabled builds, drop a `config.txt` on the
card root and the device applies it at next boot, then renames the file
to `config.ok`. The file can be empty - no key is required. A typical
Wi-Fi-only seed:

```ini
ssid_0=your-network
pass_0=your-password
```

Multiple profiles use `ssid_0` / `pass_0` through `ssid_3` / `pass_3`. Any
other config field (hostname, country, timezone, HTTP auth, etc.) can be
seeded the same way or left for the web UI to set later. The timezone
specifically takes a POSIX TZ string like `CET-1CEST,M3.5.0,M10.5.0/3` -
the web UI Config tab has user-friendly picker and auto-detect so
you do not need to write one by hand. Full key reference:
[configuration.md](configuration.md).

After the first STA profile is stored, SoftAP stays as a recovery fallback.
Set `softap_mode=forced` to keep it up alongside STA.

## 4. Open the web UI

```
http://aircannect/
```

Default login: `admin` / `aircannect`. Change it from the Config tab.

If `aircannect` does not resolve on your network, find the IP from your
router or use the SoftAP URL.

## 5. Test the CAN link

From the serial, web, or telnet management console:

```
get SerialNumber
```

A matched JSON-RPC response containing the AirSense serial number means
CAN is working end-to-end.
