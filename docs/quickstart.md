# AirCANnect Quickstart

From a supported ESP32-S3 board to a working web UI in a few minutes. Assumes
the [hardware](hardware.md) is wired up.

## 1. Install firmware

Download the `-initial.bin` matching the board from the
[latest release](https://github.com/m-kozlowski/aircannect/releases/latest).

Open [ESPWebTool](https://esptool.spacehuhn.com/) in Chrome or Edge:

1. Connect the board to the computer over USB.
2. Select **Connect** and choose its serial port.
3. Add the downloaded `-initial.bin` at address `0x0`.
4. Select **Program** and wait for flashing to finish.

To build from source instead, check out the matching release tag and use
PlatformIO:

```bash
pio run -e <profile> -t upload
```

Other storage variants are listed in [hardware.md](hardware.md).

## 2. First boot

Open the serial console at `921600` baud. The boot banner prints version,
chip, PSRAM, and storage state. Look for:

```
[BOOT] version=
```

near the start. On CAN-enabled builds, repeated `[CAN] alert:` lines with
`bus_error` or `tx_failed` indicate that the transceiver wiring, `CANH` /
`CANL` polarity, and grounds need checking.

## 3. Get on Wi-Fi

Two paths.

**SoftAP setup**  With no stored profile the device
brings up SoftAP `aircannect_XXXXXX` (password `aircannect`). Join it from
a phone or laptop, open `http://192.168.4.1/`, log in as `admin` /
`aircannect`, and use the first page of the setup wizard to add your home
Wi-Fi and choose a hostname. The connection may close when AirCANnect switches
from SoftAP to the selected network; reopen it by hostname or its new IP.

**SD provisioning**  On SD-enabled builds, drop a `config.txt` on the
card root and the device applies it at next boot, then renames the file
to `config.ok`. The file can be empty - no key is required. A typical
Wi-Fi-only seed:

```ini
ssid_0=your-network
pass_0=your-password
```

Multiple profiles use `ssid_0` / `pass_0` through `ssid_3` / `pass_3`.
Common boot settings such as `host`, `wifi_ctry`, `tz`, `http_user`,
`http_pass`, `syslog_en`, `oxi_en`, `smb_ep`, and `shq_id` can also be seeded
this way. `config.txt` uses exact NVS keys; no aliases are accepted.
Full key reference: [configuration.md](configuration.md).

After the first STA profile is stored, SoftAP stays as a recovery fallback.
Set `softap_mode=forced` to keep it up alongside STA.

## 4. Continue in the web UI

```
http://aircannect/
```

Default login: `admin` / `aircannect`. If you used SoftAP setup, you are already
in the wizard. A fresh device walks through Wi-Fi and hostname, the AS11
connection, time, access credentials, SMB, and SleepHQ. Every step is optional.
Open `http://aircannect/wizard` to run it again without resetting the stored
configuration.

If `aircannect` does not resolve on your network, find the IP from your
router or use the SoftAP URL.

## 5. Connect the AS11

The XIAO release profiles default to CAN. Connect the transceiver as described
in [hardware.md](hardware.md); no pairing is required.

The Waveshare profile defaults to BLE. Pair it from the setup wizard or the
AS11 section of the Config tab:

1. Select BLE transport and save it, then select **Pair**.
2. On the AirSense or AirCurve, open **More > myAir App** and select
   **OK, Downloaded**.
3. Select the machine from the scan results and select **Continue**.
4. Enter the four-digit code shown by the machine and select **Pair**.
