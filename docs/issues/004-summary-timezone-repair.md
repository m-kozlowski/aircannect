# Issue #4: Repairing Wrong Report Dates And Nightly Statistics

## Does This Affect Me?

This affects AirSense 11 / AirCurve 11 history saved by AirCANnect v2.6.3 or
earlier when the device timezone was UTC-5 or further west, or UTC+12 or
further east.
AirMini is not affected.

- **AirCANnect:** historical nights can appear one day late, be missing, or
  fail to open or rebuild.
- **OSCAR / SleepHQ:** nightly statistics such as AHI, leak and pressure
  percentiles can be missing or incorrect even when detailed graphs work.

## Repair Report Dates

On the corrected firmware, open the **Console** tab in the AirCANnect WebUI:

```text
report repair_dates preview
```

The preview appears automatically when ready. Every proposed date must move
**exactly one day earlier**. If any move is different, stop and paste the
output into the issue.

Example output:

```text
20261020 -> 20261019 restore
20261021 -> 20261020 archive; existing source retained
```

- `restore`: the night moves to the correct date.
- `archive; existing source retained`: the correct date already has data;
  it stays, and the shifted copy is moved to `/aircannect/report/date-repair/`.

If the console says `no dates to repair`, skip to the next section. Otherwise:

```text
report repair_dates apply
```

Completion appears automatically as `state=complete`. If it fails, paste the
error into the issue. If you closed the console before it finished,
`report repair_dates` shows the result.

## Repair Nightly Statistics

Keep AirSense / AirCurve connected. Refresh all nights it still has available:

```text
edf str refresh
```

Wait for the automatic `STR refresh complete` message.
`updated` = nights corrected.
