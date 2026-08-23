# ResMed Firmware Installation

AirCANnect can install compatible AirSense 11 and AirCurve 11 firmware from the
OTA tab or management console. This page covers the choices exposed by
AirCANnect. The container format and device protocol are documented in
[airbreak-plus](https://github.com/m-kozlowski/airbreak-plus/blob/master/docs/as11/ota_protocol.md).

Firmware installation can make the therapy device unbootable. Use an image
intended for the exact device family and firmware line, keep power stable, and
do not interrupt an active erase or programming operation. Bootloader and full
flash targets require particular care.

## Installation methods

| Method | AS11 connection | Requirement | Notes |
| --- | --- | --- | --- |
| Native RPC | CAN or BLE | Running stock or Airbreak application | Default and most compatible method. The image is uploaded and verified by the running firmware before apply. |
| Patched Bootloader | CAN only | A bootloader with the Airbreak service extension | Faster direct flash access. It cannot operate through the AS11 BLE connection. |

Use **Native RPC** unless the device already has a compatible patched
bootloader and there is a reason to use service mode.

### Stock firmware, Airbreak, and the OTA key

Native RPC uses the selected CAN or BLE application connection for upload and
verification. Applying the verified image depends on the connection:

| Method | Stock firmware availability | OTA key |
| --- | --- | --- |
| `ApplyUpgrade` | CAN only | Not required |
| `ApplyAuthenticatedUpgrade` | CAN and BLE | Required |

AirCANnect tries `ApplyUpgrade` first. If it is unavailable and an OTA key is
configured, it uses `ApplyAuthenticatedUpgrade`. Airbreak can also make
`ApplyUpgrade` available over BLE, allowing installation without an OTA key.

The OTA key is a device-specific 32-byte secret stored as 64 hexadecimal
characters in `as11_ota_key`. See
[Retrieving the local OTA key](https://github.com/m-kozlowski/airbreak-plus/blob/master/docs/as11/ota_protocol.md#retrieving-the-local-ota-key).

Without an available apply method or the required OTA key, an image may upload
and verify successfully but cannot be installed through Native RPC.

## Firmware targets

The default target is **CONF + APPL**, which updates the model definition and
main application together.

| UI name | Code | Contents |
| --- | --- | --- |
| CONF + APPL | `APCX` | Model definition and main application. Default. |
| APPL | `APPL` | Main application only. |
| CONF | `CONF` | Device model definition and available feature set. |
| Bootloader | `FGBL` | Bootloader and lower updater region. A bad image can prevent normal boot and recovery. |
| Full flash | `FGCB` | Bootloader, model definition, and application. Highest-risk target. |


## Firmware repository

The repository stores reusable images under:

```text
/aircannect/resmed-firmware
```

Use **Add** to upload raw `.bin`/`.img` images or ready `.abc` containers. The
repository recognizes supported image layouts and makes entries available for
installation. Clicking an entry downloads it; rename and delete actions manage
the stored file. A recognized bootloader-only image is filed as
`bootloaders/<bootloader-version>/patched.bin` for service-mode recovery.

The **Install another image** control uploads and installs one file without
requiring it to be added to the repository first.

## Dumping current firmware

**Dump current** reads the complete internal firmware through Patched
Bootloader service mode and saves it in the repository. This requires physical
CAN and a compatible patched bootloader. The resulting name has the form:

```text
dump-<family>-<firmware-version>_vid<NN>.bin
```

If service mode is unavailable, AirCANnect can offer to install a matching
patched bootloader through Native RPC and retry. Recovery bootloaders are kept
under `bootloaders/<bootloader-version>/patched.bin`; the directory is selected
from the bootloader identifier, not from the main application version. This
fallback is performed only after explicit confirmation.

The service protocol itself is documented in
[airbreak-plus](https://github.com/m-kozlowski/airbreak-plus/blob/master/docs/as11/bootloader_service_protocol.md).

## Typical installation

1. Stop therapy and keep the device on stable power.
2. Select **Native RPC** unless a patched bootloader is already installed.
3. Leave **CONF + APPL** selected for a normal firmware update.
4. Choose an image from the repository or upload another image.
5. Confirm the selected target and wait for verification, programming, and
   reboot to finish.

The management console exposes the same operations. Run `help resmed-ota` for
the current command forms and target names.
