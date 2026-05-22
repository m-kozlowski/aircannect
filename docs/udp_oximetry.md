# UDP Oximetry

Feed SpO2 and heart rate from external sources over UDP. This allows
integrating oximeters that AirCANnect doesn't support natively - write a small
script or app that reads your device and sends readings over the network.

## Packet format

7-byte UDP packet:

```text
[0x55] [0xAB] [flags] [spo2_lo] [spo2_hi] [hr_lo] [hr_hi]
```

| Byte | Value |
|------|-------|
| 0-1 | Magic: `0x55 0xAB` |
| 2 | Flags: `0x00` |
| 3-4 | SpO2 as 16-bit little-endian SFLOAT (e.g. 95 = `0x5F 0x00`) |
| 5-6 | Heart rate as 16-bit little-endian SFLOAT (e.g. 72 = `0x48 0x00`) |

To signal no finger / invalid reading, send `0xFF 0x07` for both fields.

The encoding is compatible with the Bluetooth PLX standard (SFLOAT), so raw BLE
notifications can be forwarded directly without conversion.

Invalid packets (wrong size, bad magic, out-of-range values) are silently
dropped.

## Config

`oxi_en` - default on, set off to disable.
`oxi_udp` - default 8025.
`oxi_adv` - default auto. Auto advertises to AS11 only while source data is
fresh.

## Source arbitration

Only one source feeds at a time.

UDP takes ownership on the first valid packet. A BLE oximeter takes ownership
when it starts notifying. The other source is ignored until the active source
goes silent.
