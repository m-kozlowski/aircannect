# Translating AirCANnect

Create a catalog named for a language tag such as `de.json` or `zh-TW.json`.
Set `language.name` to the native name of your language. Use `en.json` as the
complete template, including labels supplied by firmware. Translate as many
entries as you want; omitted entries use the English text. Existing catalogs
do not need translations of new keys before firmware can be built.

Translate values, not keys. Preserve named parameters such as `{count}` and
`{detail}`. Plural messages use the categories of `Intl.PluralRules` for your
language; `other` is required. Every variant must retain the same parameters.
Values are plain text, not HTML. Do not translate protocol identifiers,
channel names, units or product names.

Run `python3 python/tools/web_localization.py --sync` to validate catalogs and
update the firmware's language choices. No separate language list or device
endpoint needs editing. Unknown keys and mismatched parameters are errors;
missing translations are not.

Each language is served as a separate compressed resource from firmware.
Browsers load English as the fallback and only their selected translation.

English entries beginning with `config.field.`, `config.option.`,
`config.section.`, `config.action.`, `clinical.label.`, `clinical.option.`
and `clinical.group.`
may originate in firmware descriptors. Developers synchronize those with
`python3 python/tools/web_localization.py --sync --descriptors`; do not manually
change the English wording of generated entries. Other languages are always
maintained here. The synchronization never overwrites their translations.

Descriptor commands require PlatformIO (`pio` on PATH) and a host GCC C++
compiler. They build the `ui-descriptors` environment in `platformio.ini`;
PlatformIO installs its dependencies on first use. No firmware build or
private test environment is required. To build the tool directly, run
`pio run -e ui-descriptors`.

Some descriptor entries apply only to other boards or ResMed models; keep them
in the template even if your device does not display them. JSON comments are
not supported. Firmware metadata remains the source for CLI text; the complete
English catalog is also used by the browser, not just as an unused reference.

`python3 python/tools/web_localization.py --descriptors` checks descriptor
drift as well as catalog coverage and parameters.
GitHub and Gitea run this check for pull requests and before release builds.
