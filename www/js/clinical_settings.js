(() => {
    "use strict";

    let settingsData = null;
    let settingsCatalog = [];
    let settingsComposites = [];
    let settingsCatalogPromise = null;
    let settingsCatalogRevision = 0;
    let settingsProfileMode = null;
    let settingsModeDirty = false;
    let settingsActiveMode = null;

    function invalidateSettingsCatalog() {
      settingsCatalog = [];
      settingsComposites = [];
      settingsCatalogPromise = null;
      settingsCatalogRevision = 0;
      settingsProfileMode = null;
      settingsModeDirty = false;
      if (AirCANnect.pages.isActive("clinical")) loadSettings(true);
    }

    function modeBit(mode) {
      const value = Number(mode);
      if (!Number.isInteger(value) || value < 0 || value > 15) return 0;
      return 1 << value;
    }

    async function ensureSettingsCatalog(expectedRevision) {
      const expected = Number(expectedRevision || 0);
      for (let attempt = 0; attempt < 2; attempt++) {
        if (settingsCatalog.length &&
            (!expected || settingsCatalogRevision === expected)) {
          return;
        }
        if (!settingsCatalogPromise) {
          settingsCatalogPromise = AirCANnect.http.requestOk("/api/settings-catalog")
            .then((response) => response.json())
            .then((data) => {
              const catalog = Array.isArray(data.settings) ?
                data.settings : [];
              if (!catalog.some((item) => item && item.kind && item.label)) {
                throw new Error("settings catalog unavailable");
              }
              settingsCatalog = catalog.map((item, index) =>
                Object.assign({_catalogIndex: index}, item));
              settingsComposites = Array.isArray(data.composites) ?
                data.composites.map((item, index) =>
                  Object.assign({_catalogIndex: 10000 + index}, item)) : [];
              settingsCatalogRevision = Number(data.revision || 0);
            })
            .finally(() => {
              settingsCatalogPromise = null;
            });
        }
        await settingsCatalogPromise;
      }

      throw new Error("settings catalog changed during refresh");
    }

    function settingMetaFor(setting, mode) {
      const bit = modeBit(mode);
      let fallback = null;
      for (const meta of settingsCatalog) {
        if (meta.key !== setting.key) continue;
        if (!fallback) fallback = meta;
        if (bit && (Number(meta.modes || 0) & bit)) return meta;
      }
      return fallback;
    }

    function settingsTherapyMode(data) {
      const setting = (data.settings || []).find((item) =>
        item && item.key === "MOP");
      if (!setting || setting.value === undefined || setting.value === null) {
        return null;
      }
      const mode = Number(setting.value);
      return Number.isFinite(mode) ? mode : null;
    }

    function mergeSettingsCatalog(data) {
      const activeMode = settingsTherapyMode(data);
      const mode = settingsModeDirty && settingsProfileMode !== null ?
        settingsProfileMode : activeMode;
      const merged = Object.assign({}, data);
      merged.settings = (data.settings || []).map((setting) => {
        const meta = settingMetaFor(setting, mode);
        return meta ? Object.assign({}, meta, setting) : setting;
      });
      return merged;
    }

    function optionValue(item, index) {
      return item && typeof item === "object" && item.value !== undefined ?
        String(item.value) : String(index);
    }

    function optionLabel(item) {
      return item && typeof item === "object" ?
        (item.label || String(item.value)) : String(item);
    }

    function visibleOptions(setting, supportedMask) {
      const options = setting.options || [];
      if (setting.key !== "MOP" || !supportedMask) return options;
      return options.filter((item, index) =>
        supportedMask & modeBit(optionValue(item, index)));
    }

    function settingRawValue(setting) {
      if (!setting) return "";
      if (setting.pending) return setting.pending_value || "";
      return setting.value || "";
    }

    function compositeOptionValue(item) {
      if (!item || typeof item !== "object") return "";
      if (item.numeric_raw !== undefined && item.numeric_raw !== null) {
        return "n:" + String(item.numeric_raw);
      }
      return "e:" + String(item.enum_value);
    }

    function compositeOptionLabel(item) {
      return item && typeof item === "object" ?
        (item.label || compositeOptionValue(item)) : String(item);
    }

    function compositeCurrentValue(composite, enumSetting, numericSetting) {
      const enumRaw = String(settingRawValue(enumSetting));
      const branch = String(composite.numeric_branch_enum_value);
      if (enumRaw === branch) {
        const numericRaw = String(settingRawValue(numericSetting));
        if (numericRaw !== "") return "n:" + numericRaw;
      }
      return "e:" + enumRaw;
    }

    function compositeDisplayFallback(value) {
      if (String(value).startsWith("n:")) return String(value).slice(2);
      if (String(value).startsWith("e:")) return String(value).slice(2);
      return String(value || "");
    }

    function formatSettingValue(setting, value) {
      if (value === undefined || value === null || value === "") return "";
      const raw = String(value);
      if (setting.kind === "composite") {
        const options = setting.options || [];
        for (let i = 0; i < options.length; i++) {
          if (optionValue(options[i], i) === raw) {
            return optionLabel(options[i]) || raw;
          }
        }
        return compositeDisplayFallback(raw);
      }
      if (setting.kind === "enum") {
        const options = setting.options || [];
        for (let i = 0; i < options.length; i++) {
          if (optionValue(options[i], i) === raw) {
            return optionLabel(options[i]) || raw;
          }
        }
        return raw;
      }
      if (setting.kind === "number") {
        const numeric = Number(raw);
        if (!Number.isFinite(numeric)) return raw;
        const scale = Number(setting.scale_div || 1);
        const displayValue = scale > 1 ? numeric / scale : numeric;
        const decimals = Number(setting.decimals || 0);
        if (Number.isInteger(decimals) && decimals >= 0) {
          return displayValue.toFixed(decimals);
        }
        return String(displayValue);
      }
      return raw;
    }

    function settingAvailable(setting) {
      return setting.available !== false;
    }

    function settingWritable(setting) {
      return setting.writable !== false;
    }

    function settingSmallText(setting) {
      const parts = [];
      if (setting.rpc_name) parts.push(setting.rpc_name);

      if (setting.pending) {
        parts.push("Pending readback; current " +
          (formatSettingValue(setting, setting.value) || "--"));
      } else if (setting.inferred && setting.key !== "MOP") {
        parts.push("Inferred from device status");
      }

      return parts.join("; ");
    }

    function settingVisibleForDevice(setting, settingsByKey) {
      if (setting.key === "PHT" || setting.key === "PHI") {
        const heightUnits = settingsByKey.get("IHU");
        if (!heightUnits || !settingAvailable(heightUnits)) return false;

        const expected = setting.key === "PHI" ? "1" : "0";
        return String(settingRawValue(heightUnits)) === expected;
      }

      if (setting.key === "IMN" || setting.key === "IMX" ||
          setting.key === "EPI") {
        const autoEpap = settingsByKey.get("IEU");
        if (!autoEpap || !settingAvailable(autoEpap)) {
          return setting.key === "EPI";
        }

        const enabled = String(settingRawValue(autoEpap)) === "1";
        return setting.key === "EPI" ? !enabled : enabled;
      }

      return true;
    }

    function settingGroupRank(group) {
      const order = {
        Therapy: 0,
        Comfort: 1,
        Circuit: 2,
        Configuration: 3,
        Preferences: 4,
        Device: 5,
      };
      return Object.prototype.hasOwnProperty.call(order, group) ?
        order[group] : 100;
    }

    function settingCategoryRank(category) {
      const order = {
        therapy: 0,
        pressure: 1,
        pressure_support: 2,
        backup_rate: 3,
        comfort: 4,
        timing: 5,
        trigger_cycle: 6,
        therapy_start_pressure: 10,
        ramp: 11,
        therapy_behavior: 12,
        climate: 13,
        circuit: 20,
        locale: 30,
        patient_access: 31,
        display: 32,
      };
      return Object.prototype.hasOwnProperty.call(order, category) ?
        order[category] : 100;
    }

    function compareSettings(a, b) {
      const groupDelta =
        settingGroupRank(a.group || "") - settingGroupRank(b.group || "");
      if (groupDelta) return groupDelta;

      const categoryDelta =
        settingCategoryRank(a.category || "") -
        settingCategoryRank(b.category || "");
      if (categoryDelta) return categoryDelta;

      return Number(a._catalogIndex || 0) - Number(b._catalogIndex || 0);
    }

    function buildCompositeSettings(settings) {
      const byKey = new Map();
      settings.forEach((setting) => byKey.set(setting.key, setting));

      const hiddenKeys = new Set();
      const composites = [];
      settingsComposites.forEach((composite) => {
        if (composite.kind !== "paired_enum_numeric") return;

        const enumSetting = byKey.get(composite.enum_key);
        const numericSetting = byKey.get(composite.numeric_key);
        if (!enumSetting || !numericSetting) return;

        const available =
          (settingAvailable(enumSetting) || enumSetting.pending) &&
          (settingAvailable(numericSetting) || numericSetting.pending);
        if (!available) return;

        const options = (composite.options || []).map((item) =>
          Object.assign({}, item, {value: compositeOptionValue(item)}));
        const value = compositeCurrentValue(
          composite, enumSetting, numericSetting);

        hiddenKeys.add(composite.enum_key);
        hiddenKeys.add(composite.numeric_key);
        composites.push(Object.assign({}, composite, {
          kind: "composite",
          value,
          pending_value: value,
          options,
          pending: enumSetting.pending || numericSetting.pending,
          writable: settingWritable(enumSetting) &&
            settingWritable(numericSetting),
          available: true,
          enum_setting: enumSetting,
          numeric_setting: numericSetting,
          _catalogIndex: Math.min(
            Number(enumSetting._catalogIndex || 0),
            Number(numericSetting._catalogIndex || 0)),
        }));
      });

      return {hiddenKeys, composites};
    }

    function settingDisplayNumber(settingsByKey, key) {
      const setting = settingsByKey.get(key);
      if (!setting || !settingAvailable(setting)) return null;

      const value = Number(formatSettingValue(
        setting, settingRawValue(setting)));
      return Number.isFinite(value) ? value : null;
    }

    function settingRawNumber(settingsByKey, key) {
      const setting = settingsByKey.get(key);
      if (!setting || !settingAvailable(setting)) return null;

      const value = Number(settingRawValue(setting));
      return Number.isFinite(value) ? value : null;
    }

    function catalogIndexAfter(settingsByKey, key, offset) {
      const setting = settingsByKey.get(key);
      if (!setting) return Number.MAX_SAFE_INTEGER;
      return Number(setting._catalogIndex || 0) + offset;
    }

    function derivedSetting(key, label, value, category, catalogIndex, unit,
                            decimals) {
      return {
        key,
        label,
        value: String(value),
        group: "Therapy",
        category,
        kind: decimals === null ? "text" : "number",
        scale_div: 1,
        decimals: decimals === null ? 0 : decimals,
        unit,
        writable: false,
        available: true,
        _catalogIndex: catalogIndex,
      };
    }

    function ieRatioPercent(rate, inspiratoryTime) {
      if (!(rate > 0) || !(inspiratoryTime > 0)) return null;

      const cycle = 60 / rate;
      const expiratoryTime = cycle - inspiratoryTime;
      if (!(expiratoryTime > 0)) return null;

      return Math.round(100 * inspiratoryTime / expiratoryTime);
    }

    function formatIeRatio(percent) {
      if (!(percent > 0)) return "";

      const ratio = percent < 100 ? 100 / percent : percent / 100;
      const value = ratio.toFixed(1).replace(/\.0$/, "");
      return percent < 100 ? "1:" + value : value + ":1";
    }

    function buildDerivedSettings(settings, mode) {
      const byKey = new Map();
      settings.forEach((setting) => byKey.set(setting.key, setting));

      if (mode === 9) {
        const heightUnit = settingRawNumber(byKey, "IHU");
        let heightCm = null;
        if (heightUnit === 0) {
          heightCm = settingDisplayNumber(byKey, "PHT");
        } else if (heightUnit === 1) {
          const heightInches = settingDisplayNumber(byKey, "PHI");
          if (heightInches !== null) heightCm = heightInches * 2.54;
        }

        const rate = settingDisplayNumber(byKey, "IBR");
        const targetVa = settingDisplayNumber(byKey, "ITV");
        const tiMin = settingDisplayNumber(byKey, "IVN");
        const tiMax = settingDisplayNumber(byKey, "IVX");
        const derived = [];

        if (heightCm !== null && heightCm > 0 &&
            rate !== null && rate > 0 && targetVa !== null) {
          const deadspace = Math.pow(heightCm / 175, 2.363) * 0.12;
          const minuteVentilation = targetVa + rate * deadspace;
          const tidalVolume = minuteVentilation / rate * 1000;
          const referenceWeight = heightCm < 130 ?
            14.4 + (heightCm - 100) * 0.403 :
            48 + (heightCm - 152.4) * 0.91;

          derived.push(derivedSetting(
            "derived_ivaps_mv", "MV", minuteVentilation,
            "ivaps_therapy", catalogIndexAfter(byKey, "ITV", 0.1),
            "L/min", 1));
          derived.push(derivedSetting(
            "derived_ivaps_vt", "Vt", tidalVolume,
            "ivaps_therapy", catalogIndexAfter(byKey, "ITV", 0.2),
            "mL", 0));
          if (referenceWeight > 0) {
            derived.push(derivedSetting(
              "derived_ivaps_vt_per_kg", "Vt/kg",
              tidalVolume / referenceWeight,
              "ivaps_therapy", catalogIndexAfter(byKey, "ITV", 0.3),
              "mL/kg", 1));
          }
        }

        const ieMin = ieRatioPercent(rate, tiMin);
        const ieMax = ieRatioPercent(rate, tiMax);
        if (ieMin !== null && ieMax !== null) {
          derived.push(derivedSetting(
            "derived_ivaps_ie", "I:E",
            formatIeRatio(ieMin) + "–" + formatIeRatio(ieMax),
            "ivaps_therapy", catalogIndexAfter(byKey, "IVX", 0.1),
            "", null));
        }
        return derived;
      }

      if (mode === 10) {
        const rate = settingDisplayNumber(byKey, "PA6");
        const inspiratoryTime = settingDisplayNumber(byKey, "PA5");
        const ratio = ieRatioPercent(rate, inspiratoryTime);
        return ratio === null ? [] : [derivedSetting(
          "derived_pac_ie", "I:E", formatIeRatio(ratio),
          "pac_therapy", catalogIndexAfter(byKey, "PA6", 0.1), "", null)];
      }

      return [];
    }

    async function applySettingsSnapshot(data) {
      await ensureSettingsCatalog(data.catalog_revision);
      settingsData = mergeSettingsCatalog(data);
      renderSettings(settingsData);
    }

    async function loadSettings(refresh) {
      try {
        const query = [];
        if (refresh) query.push("refresh=1");
        if (settingsModeDirty && settingsProfileMode !== null) {
          query.push("profile_mode=" + encodeURIComponent(settingsProfileMode));
        }

        const response = await AirCANnect.http.requestOk("/api/settings" +
          (query.length ? "?" + query.join("&") : ""));
        const data = await response.json();
        await applySettingsSnapshot(data);
        if (!refresh && data.as11_state !== "unavailable" && !data.valid &&
            !data.refresh_queued && !data.snapshot_pending) {
          await loadSettings(true);
        }
      } catch (error) {
        AirCANnect.ui.message("settingsMsg", error.message, false);
      }
    }

    function renderSettings(data) {
      const parts = [];
      const activeMode = settingsTherapyMode(data);
      if (activeMode !== null) settingsActiveMode = activeMode;
      if (!settingsModeDirty && settingsActiveMode !== null) {
        settingsProfileMode = settingsActiveMode;
      }
      if (settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode === settingsActiveMode && !data.pending_count) {
        settingsModeDirty = false;
      }
      const unavailable = data.as11_state === "unavailable";
      if (unavailable) {
        parts.push("Device unavailable");
      } else if (data.refresh_queued) {
        parts.push("Refreshing settings");
      } else if (!data.valid) {
        parts.push("Waiting for device");
      }
      if (settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode !== settingsActiveMode) {
        parts.push("Unsaved mode change");
      }
      if (data.pending_count) parts.push(data.pending_count + " pending readback");

      const meta = document.getElementById("settingsMeta");
      const metaText = parts.join("; ");
      meta.textContent = metaText;
      meta.style.display = metaText ? "block" : "none";

      const root = document.getElementById("settingsFields");
      const save = document.getElementById("settingsSave");
      const revert = document.getElementById("settingsRevert");
      root.innerHTML = "";
      if (revert) {
        revert.style.display =
          settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode !== settingsActiveMode ? "" : "none";
      }

      const baseVisible = (data.settings || []).filter((setting) =>
        settingAvailable(setting) || setting.inferred || setting.pending);
      const displayMode = settingsModeDirty && settingsProfileMode !== null ?
        settingsProfileMode : activeMode;
      const displaySettings = baseVisible.concat(
        buildDerivedSettings(baseVisible, displayMode));
      const settingsByKey = new Map();
      displaySettings.forEach((setting) =>
        settingsByKey.set(setting.key, setting));
      const conditionVisible = displaySettings.filter((setting) =>
        settingVisibleForDevice(setting, settingsByKey));
      const compositeState = buildCompositeSettings(conditionVisible);
      const visible = conditionVisible.filter((setting) =>
          !compositeState.hiddenKeys.has(setting.key))
        .concat(compositeState.composites)
        .sort(compareSettings);
      if (!visible.length) {
        root.innerHTML = '<div class="value" style="text-align:left">' +
          (unavailable ? "Device unavailable" :
            data.valid ? "No readable settings for this mode" :
              "Waiting for AS11 settings readback") +
          "</div>";
        if (save) save.disabled = true;
        return;
      }

      let group = "";
      visible.forEach((setting) => {
        const settingGroup = setting.group || "";
        if (settingGroup !== group) {
          group = settingGroup;
          const heading = document.createElement("div");
          heading.className = "section-title";
          heading.textContent = group || "settings";
          root.appendChild(heading);
        }

        let raw = setting.pending ?
          setting.pending_value : (setting.value || "");
        if (setting.key === "MOP" && settingsModeDirty &&
            settingsProfileMode !== null) {
          raw = String(settingsProfileMode);
        }
        const shown = formatSettingValue(setting, raw);
        let control;

        if (!settingWritable(setting)) {
          const unit = setting.unit ? " " + setting.unit : "";
          control = AirCANnect.ui.valueSpan((shown || "--") + (shown ? unit : ""));
        } else if (setting.kind === "enum" || setting.kind === "composite") {
          control = document.createElement("select");
          let seen = false;
          if (raw === "") {
            const option = document.createElement("option");
            option.value = "";
            option.textContent = "--";
            option.selected = true;
            option.disabled = true;
            control.appendChild(option);
          }
          visibleOptions(setting, data.supported_mode_mask || 0)
            .forEach((item, index) => {
              const label = setting.kind === "composite" ?
                compositeOptionLabel(item) : optionLabel(item);
              const value = optionValue(item, index);
              const option = document.createElement("option");
              option.value = value;
              option.textContent = label;
              if (setting.kind === "composite") {
                option.dataset.enumValue = item.enum_value;
                if (item.numeric_raw !== undefined &&
                    item.numeric_raw !== null) {
                  option.dataset.numericRaw = item.numeric_raw;
                }
              }
              if (value === String(raw)) {
                option.selected = true;
                seen = true;
              }
              control.appendChild(option);
            });
          if (raw !== "" && !seen) {
            const option = document.createElement("option");
            option.value = raw;
            option.textContent = shown || raw;
            option.selected = true;
            control.appendChild(option);
          }
          if (setting.key === "MOP") {
            control.onchange = () => {
              const nextMode = Number(control.value);
              if (!Number.isNaN(nextMode)) {
                settingsProfileMode = nextMode;
                settingsModeDirty = settingsActiveMode !== null &&
                  nextMode !== settingsActiveMode;
                loadSettings(false);
              }
            };
          }
        } else if (setting.kind === "bool") {
          control = document.createElement("select");
          ["false", "true"].forEach((value) => {
            const option = document.createElement("option");
            option.value = value;
            option.textContent = value === "true" ? "On" : "Off";
            const current = String(raw).toLowerCase();
            if (current === value ||
                current === (value === "true" ? "on" : "off")) {
              option.selected = true;
            }
            control.appendChild(option);
          });
        } else {
          control = document.createElement("input");
          control.type = setting.kind === "number" ? "number" : "text";
          control.value = settingAvailable(setting) || setting.inferred ?
            (shown || "") : "";
          if (setting.kind === "number") {
            control.min = setting.min;
            control.max = setting.max;
            control.step = setting.step;
          }
        }

        if (settingWritable(setting)) {
          control.dataset.key = setting.key;
          control.dataset.kind = setting.kind;
          if (setting.kind === "composite") {
            control.dataset.enumKey = setting.enum_key;
            control.dataset.numericKey = setting.numeric_key;
            control.dataset.numericScaleDiv =
              setting.numeric_setting && setting.numeric_setting.scale_div ?
                setting.numeric_setting.scale_div : 1;
          }
          control.dataset.orig =
            setting.kind === "enum" || setting.kind === "bool" ?
              (raw || "") : (control.value || "");
          if (setting.kind === "composite") control.dataset.orig = raw || "";
          if (!settingAvailable(setting) || unavailable) {
            control.disabled = true;
          }
          if (setting.pending) control.classList.add("pending");
        }

        const entry = AirCANnect.ui.row(
          setting.label || setting.key,
          control,
          settingSmallText(setting));
        if (setting.pending) entry.classList.add("pending");
        root.appendChild(entry);
      });

      if (save) save.disabled = !root.querySelector("[data-key]:not(:disabled)");
    }

    async function saveSettings() {
      const changes = {};
      if (settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode !== settingsActiveMode) {
        changes.MOP = settingsProfileMode;
      }
      document.querySelectorAll("#settingsFields [data-key]").forEach((input) => {
        if (input.disabled) return;
        if (String(input.value) === String(input.dataset.orig)) return;

        if (input.dataset.kind === "number") {
          if (input.value === "") return;
          changes[input.dataset.key] = Number(input.value);
        } else if (input.dataset.kind === "bool") {
          changes[input.dataset.key] = input.value === "true";
        } else if (input.dataset.kind === "enum") {
          if (input.value === "") return;
          changes[input.dataset.key] = Number(input.value);
        } else if (input.dataset.kind === "composite") {
          const selected = input.selectedOptions && input.selectedOptions[0];
          if (!selected || selected.dataset.enumValue === undefined) return;

          changes[input.dataset.enumKey] = Number(selected.dataset.enumValue);
          if (selected.dataset.numericRaw !== undefined) {
            const numeric = Number(selected.dataset.numericRaw);
            const scale = Number(input.dataset.numericScaleDiv || 1);
            changes[input.dataset.numericKey] =
              scale > 1 ? numeric / scale : numeric;
          }
        } else {
          changes[input.dataset.key] = input.value;
        }
      });

      if (!Object.keys(changes).length) {
        AirCANnect.ui.message("settingsMsg", "No changes", true);
        return;
      }

      try {
        const response = await AirCANnect.http.requestOk("/api/settings", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(changes),
        });
        const data = await response.json();
        AirCANnect.ui.message("settingsMsg",
          data.ok ? "Settings queued" : "Queue failed",
          data.ok);
      } catch (error) {
        AirCANnect.ui.message("settingsMsg", error.message, false);
      }
    }

    function revertSettingsDraft() {
      if (settingsActiveMode === null) return;
      settingsProfileMode = settingsActiveMode;
      settingsModeDirty = false;
      loadSettings(false);
    }


    AirCANnect.actions.register("settings.save", () => saveSettings());
    AirCANnect.actions.register("settings.revert", () =>
      revertSettingsDraft());
    AirCANnect.actions.register("settings.refresh", () => loadSettings(true));
    AirCANnect.pages.onLoad("clinical", (refresh) => loadSettings(refresh));
    AirCANnect.events.subscribe("settings", async (data) => {
      if (!AirCANnect.pages.isActive("clinical")) return;
      try {
        await applySettingsSnapshot(data);
      } catch (error) {
        AirCANnect.ui.message("settingsMsg", error.message, false);
      }
    });
    AirCANnect.events.subscribe("device_boot", () => {
      invalidateSettingsCatalog();
    }, {raw: true});
})();
